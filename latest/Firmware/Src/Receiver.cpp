/*
  Copyright (c) 2016-2020 Peter Antypas

  This file is part of the MAIANA™ transponder firmware.

  The firmware is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>
 */


#include "Receiver.hpp"
#include "EZRadioPRO.h"
#include "Events.hpp"
#include "EventQueue.hpp"
#include "NoiseFloorDetector.hpp"
#include "bsp.hpp"
#include "Stats.hpp"


Receiver::Receiver(GPIO_TypeDef *sdnPort, uint32_t sdnPin, GPIO_TypeDef *csPort, uint32_t csPin,
    GPIO_TypeDef *dataPort, uint32_t dataPin,
    GPIO_TypeDef *clockPort, uint32_t clockPin, int chipId)
: RFIC(sdnPort, sdnPin, csPort, csPin, dataPort, dataPin, clockPort, clockPin, chipId)
{
  mSlotBitNumber = -1;
  mOneBitCount = 0;
  mChannel = CH_88;
  mBitCount = 0;
  mBitState = BIT_STATE_PREAMBLE_SYNC;
  mLastNRZIBit=0x00;
  mNextChannel = mChannel;
  mRXByte = 0;
  mBitWindow = 0;
  mRXPacket = EventPool::instance().newRXPacket();
  ASSERT_VALID_PTR(mRXPacket);
}

Receiver::~Receiver()
{
}

VHFChannel Receiver::channel()
{
  return mChannel;
}

bool Receiver::init()
{
  configure();
  resetBitScanner();
  return true;
}

void Receiver::startReceiving(VHFChannel channel, bool reconfigGPIOs)
{
  mChannel = channel;
  mNextChannel = channel;
  startListening(mChannel, reconfigGPIOs);
  resetBitScanner();
}

void Receiver::switchToChannel(VHFChannel channel)
{
  mNextChannel = channel;
}

// TODO: This is a really, really long operation - over 320us !!!
void Receiver::startListening(VHFChannel channel, bool reconfigGPIOs)
{
  if ( reconfigGPIOs )
    {
      // This takes about 140us
      configureGPIOsForRX();
    }

  mChannel = channel;
  RX_OPTIONS options;
  options.channel = AIS_CHANNELS[channel].ordinal;
  options.condition = 0;
  options.rx_len = 0;
  options.next_state1 = 0;
  options.next_state2 = 0;
  options.next_state3 = 0;

  /**
   * This never takes more than 65us now :D
   */
  //bsp_signal_high();
  sendCmdNoWait(START_RX, &options, sizeof options);//, NULL, 0);
  //bsp_signal_low();
}

void Receiver::resetBitScanner()
{
  mBitWindow = 0;
  mBitCount = 0;
  mOneBitCount = 0;
  mLastNRZIBit = 0xff;
  mRXByte = 0;
  mBitState = BIT_STATE_PREAMBLE_SYNC;
  if ( mRXPacket )
    mRXPacket->reset();
}

/*
 * TODO: Under a worst case scenario, this interrupt service method
 * can take up to 320us to complete (that's 4 clock bits!!!)
 *
 * Re-architecting will be necessary to resolve this.
 */

void Receiver::onBitClock()
{
  //bsp_signal_high();
  ++mSlotBitNumber;

  // Don't waste time processing bits when the transceiver is transmitting
  if ( gRadioState == RADIO_TRANSMITTING )
    {
      //bsp_signal_low();
      return;
    }


  if ( !mRXPacket )
    {
      mRXPacket = EventPool::instance().newRXPacket();
      if ( !mRXPacket )
        {
          return;
        }
    }


  uint8_t bit = HAL_GPIO_ReadPin(mDataPort, mDataPin);
  Receiver::Action action = processNRZIBit(bit);
  if ( action == RESTART_RX )
    {
      startReceiving(mChannel, false);
    }
#if ENABLE_TX
  /**
   * This trick ensures that we only sample RSSI every 17 time slots and never in the
   * same time slot for both ICs, so we don't conduct long SPI operations on consecutive
   * interrupt handlers that might exceed the bit clock period. There is no reason for RSSI
   * collection to have a high duty cycle anyway, it just serves to establish the noise floor.
   */
  else if ( mTimeSlot != 0xffffffff && mSlotBitNumber != 0xffff &&
      mTimeSlot % 17 == mChipID && mSlotBitNumber == CCA_SLOT_BIT - 1 )
    {
      reportRSSI();
    }
#endif

  //bsp_signal_low();
}

/**
 * This is called from the SOTDMA timer interrupt, which is at the same priority as the bit clock.
 * So timeSlotStarted() and onBitClock() cannot preempt each other.
 */

void Receiver::timeSlotStarted(uint32_t slot)
{
  // This should never be called while transmitting. Transmissions start after the slot boundary and end before the end of it.
  ASSERT(gRadioState == RADIO_RECEIVING);

  mSlotBitNumber = -1;
  mTimeSlot = slot;
  if ( mBitState == BIT_STATE_IN_PACKET )
    return;

  if ( mRXPacket )
    mRXPacket->setSlot(slot);

  if ( mChannel != mNextChannel )
    {
      startReceiving(mNextChannel, false);
    }
}

/**
 * This method must complete in a few microseconds, worst case!
 */
Receiver::Action Receiver::processNRZIBit(uint8_t bit)
{
  if ( mLastNRZIBit == 0xff )
    {
      mLastNRZIBit = bit;
      return NO_ACTION;
    }

  uint8_t decodedBit = !(mLastNRZIBit ^ bit);

  switch (mBitState) {
  case BIT_STATE_PREAMBLE_SYNC:
    {
      mLastNRZIBit = bit;
      mBitWindow <<= 1;
      mBitWindow |= decodedBit;

      /*
       * By checking for the last few preamble bits plus the HDLC start flag,
       * we gain enough confidence that this is not random noise.
       */
      if ( mBitWindow == 0b1010101001111110 || mBitWindow == 0b0101010101111110 )
        {
          mBitState = BIT_STATE_IN_PACKET;
          mRXPacket->setChannel(mChannel);
        }

      break;
    }
  case BIT_STATE_IN_PACKET:
    {
      if ( mRXPacket->size() >= MAX_AIS_RX_PACKET_SIZE )
        {
          // Start over
          return RESTART_RX;
        }

      // We can never have 7 consecutive "1" bits in a proper NRZI encoded packet
      if ( mOneBitCount >= 7 )
        {
          return RESTART_RX;
        }

      mLastNRZIBit = bit;
      mBitWindow <<= 1;
      mBitWindow |= decodedBit;

      if ( (mBitWindow & 0x00ff) == 0x7E )
        {
          // We have a complete packet
          mBitState = BIT_STATE_PREAMBLE_SYNC;
          /**
           * This is the longest operation undertaken here. Now that we use object pools and pointers,
           * it completes in about 14us
           */
          pushPacket();
          return RESTART_RX;
        }
      else
        {
          addBit(decodedBit);
        }

      break;
    }
  }

  return NO_ACTION;
}


bool Receiver::addBit(uint8_t bit)
{
  bool result = true;
  if ( bit )
    {
      ++mOneBitCount;
    }
  else
    {
      // Don't put stuffed bits into the packet
      if ( mOneBitCount == 5 )
        result = false;

      mOneBitCount = 0;
    }

  if ( result )
    {
      mRXByte <<= 1;
      mRXByte |= bit;
      ++mBitCount;
    }

  if ( mBitCount == 8 )
    {
      // Commit to the packet!
      mRXPacket->addByte(mRXByte);
      mBitCount = 0;
      mRXByte = 0;
    }

  return result;
}

void Receiver::pushPacket()
{
  Event *p = EventPool::instance().newEvent(AIS_PACKET_EVENT);
  ASSERT_VALID_PTR(p);

  if ( p )
    {
      //bsp_signal_high();
      p->rxPacket = mRXPacket;
      if ( !EventQueue::instance().push(p) )
        {
          // Count this
          ++Stats::instance().eventQueuePushFailures;
        }
      //bsp_signal_low();
      mRXPacket = EventPool::instance().newRXPacket();
      if ( !mRXPacket )
        {
          // TODO: Count this
          ++Stats::instance().rxPacketPoolPopFailures;
        }
    }
  else
    {
      // TODO: Count this
      ++Stats::instance().eventQueuePopFailures;
      /**
       * We're out of resources so just keep using the existing packet.
       * If this happens, the most logical outcome is a watchdog reset
       * because something has blocked the main task and the pool is not
       * getting replenished
       */
      mRXPacket->reset();
    }
}

/**
 * This operation typically takes under 85us
 */
uint8_t Receiver::reportRSSI()
{
  //bsp_signal_high();
  uint8_t rssi = readRSSI();
  char channel = AIS_CHANNELS[mChannel].designation;
  NoiseFloorDetector::instance().report(channel, rssi);
  //bsp_signal_low();
  return rssi;
}

void Receiver::configureGPIOsForRX()
{
  GPIO_PIN_CFG_PARAMS gpiocfg;
  gpiocfg.GPIO0 = 0x00;       // No change
  gpiocfg.GPIO1 = 0x14;       // RX data bits
  gpiocfg.GPIO2 = 0x00;       // No change
  gpiocfg.GPIO3 = 0x1F;       // RX/TX data clock
  gpiocfg.NIRQ  = 0x00;       // Nothing
  gpiocfg.SDO   = 0x00;       // No change
  gpiocfg.GENCFG = 0x00;      // No change
  sendCmd(GPIO_PIN_CFG, &gpiocfg, sizeof gpiocfg, NULL, 0);
}


