/*
 * SerialToTCPBridgeProtocol.c
 *
 *  Created on: Jul 29, 2016
 *      Author: Roan
 */

#include "SerialToTCPBridgeProtocol.h"

// Private Methods
static int tx_available(Client* c)
{
	if (c->txFull)
	{
		return 256;
	}
	if (c->pTail_tx >= c->pHead_tx)
	{
		return (int) (c->pTail_tx - c->pHead_tx);
	} else
	{
		return 256 - (int) (c->pHead_tx - c->pTail_tx);
	}
}

static bool initiate_uart_tx(Client* c)
{
	HAL_StatusTypeDef txStatus;
	if ((int) (c->pHead_tx) + tx_available(c) > 256)
	{
		txStatus = HAL_UART_Transmit_IT(c->peripheral_UART, &c->txBuffer[c->pHead_tx], 256 - c->pHead_tx);
		if (txStatus == HAL_OK)
		{
			c->pHead_tx = 0;
		} else
		{
			return false;
		}
	} else
	{
		int have = tx_available(c);
		txStatus = HAL_UART_Transmit_IT(c->peripheral_UART, &c->txBuffer[c->pHead_tx], (uint16_t)have);
		if (txStatus == HAL_OK)
		{
			c->pHead_tx += (uint8_t)have;
		} else
		{
			return false;
		}
	}
	c->txReady = false;
	c->txFull = false;
	return true;
}

static bool writePacket(Client* c, uint8_t command, uint8_t* payload, uint8_t pLength)
{
	c->workBuffer[0] = pLength + 5;
	c->workBuffer[1] = command;
	if (payload != NULL)
	{
		for (uint8_t i = 2; i < pLength + 2; i++)
		{
			c->workBuffer[i] = payload[i - 2];
		}
	}
	uint32_t crcCode = HAL_CRC_Calculate(c->peripheral_CRC, (uint32_t*) (c->workBuffer), pLength + 2);
	crcCode ^= 0xffffffff;
	c->workBuffer[pLength + 2] = crcCode & 0x000000FF;
	c->workBuffer[pLength + 3] = (crcCode & 0x0000FF00) >> 8;
	c->workBuffer[pLength + 4] = (crcCode & 0x00FF0000) >> 16;
	c->workBuffer[pLength + 5] = (crcCode & 0xFF000000) >> 24;

	// see if packet will fit in transmit buffer
	if ((int) (pLength) + 6 > 256 - tx_available(c))
	{
		return 0;
	}

	// write packet into transmit buffer
	for (int i = 0; i < pLength + 6; i++)
	{
		c->txBuffer[c->pTail_tx++] = c->workBuffer[i];
	}
	c->txFull = (c->pTail_tx == c->pHead_tx);

	if (c->txReady)
	{
		if (!initiate_uart_tx(c))
		{
			return false;
		}
	}
	return true;
}

static size_t publish(Client* c, uint8_t* payload, uint8_t pLength)
{
	uint8_t cmdSequence = PROTOCOL_PUBLISH;
	if (c->sequenceTxFlag)
	{
		cmdSequence |= 0x80;
	}
	if (!writePacket(c, cmdSequence, payload, pLength))
	{
		return 0;
	}
	c->ackOutstanding = true;
	return pLength;
}

static void rxHandlePacket(Client* c, uint8_t* packetStart)
{
	bool rxSeqFlag = (packetStart[1] & 0x80) > 0;
	switch (packetStart[1] & 0x7F)
	{
	// Connection established with destination
	case PROTOCOL_CONNACK:
		if (packetStart[0] == 5)
		{
			c->state = STATE_CONNECTED;
		}
		break;
	// Incoming data
	case PROTOCOL_PUBLISH:
		writePacket(c, PROTOCOL_ACK | (packetStart[1] & 0x80), NULL, 0);
		if (rxSeqFlag == c->expectedRxSeqFlag)
		{
			c->expectedRxSeqFlag = !c->expectedRxSeqFlag;
			if (packetStart[0] > 5) // how can this work? is workBuffer not overwritten with writePacket?
			{
				for (uint8_t i = 0; i < packetStart[0] - 5; i++)
				{
					c->readBuffer[c->pRx_read++] = packetStart[2 + i];
				}
				c->readFull = (c->pRead_read == c->pRx_read);
			}
		}
		break;
	// Protocol Acknowledge
	case PROTOCOL_ACK:
		if (rxSeqFlag == c->sequenceTxFlag)
		{
			c->sequenceTxFlag = !c->sequenceTxFlag;
			c->ackOutstanding = false;
		}
		break;
	}
}

// Callback hook ups
void uartTxCompleteCallback(Client* c)
{
	// if leftover, initiate send for next 64byte max?
	if (tx_available(c) > 0)
	{
		initiate_uart_tx(c);
	} else
	{
		c->txReady = true;
	}

}

// TODO: Packet RX timeout, buffer full check?
void uartRxCompleteCallback(Client* c)
{
	static uint8_t packetCount = 0;
	static uint8_t rxState = RX_PACKET_IDLE;

	c->rxBuffer[packetCount++] = c->rxByte;
	switch (rxState)
	{
	case RX_PACKET_IDLE:
		rxState = RX_PACKET_GOTLENGTH;
		break;
	case RX_PACKET_GOTLENGTH:
		rxState = RX_PACKET_GOTCOMMAND;
		break;
	case RX_PACKET_GOTCOMMAND:
		; // has to be here, otherwise 'deceleration after label' error
		uint8_t packetLength = c->rxBuffer[0];
		if (packetCount == packetLength + 1) // Got rest of packet
		{
			packetCount = 0;
			// Integrity checking
			uint32_t crcRx = c->rxBuffer[packetLength - 3] | (c->rxBuffer[packetLength - 2] << 8)
					| (c->rxBuffer[packetLength - 1] << 16) | (c->rxBuffer[packetLength] << 24);
			uint32_t crcCode = HAL_CRC_Calculate(c->peripheral_CRC, (uint32_t*) (c->rxBuffer), packetLength - 3);
			crcCode ^= 0xffffffff;
			if (crcRx == crcCode) // valid packet
			{
				rxHandlePacket(c, c->rxBuffer);
			}
			rxState = RX_PACKET_IDLE;
		}
		break;
	}
	HAL_UART_Receive_IT(c->peripheral_UART, &c->rxByte, 1);
}

// Public Methods
static int availablePublic(const void* c)
{
	Client* self = (Client*)c;

	if (self->readFull)
	{
		return 256;
	}
	if (self->pRx_read >= self->pRead_read)
	{
		return (int) (self->pRx_read - self->pRead_read);
	} else
	{
		return 256 - (int) (self->pRead_read - self->pRx_read);
	}
}

static int readPublic(const void* c)
{
	Client* self = (Client*)c;

	if (!self->available(c))
	{
		return -1;
	}
	uint8_t ch = self->readBuffer[self->pRead_read++];
	self->readFull = false;
	return ch;
}

static int connectPublic(const void* c, uint8_t ip[4], uint16_t port)
{
	Client* self = (Client*)c;
	HAL_UART_Receive_IT(self->peripheral_UART, &self->rxByte, 1); // start rx on hardware uart interface
	uint8_t destination[6] = { ip[0], ip[1], ip[2], ip[3], (uint8_t)port, (uint8_t)(port >> 8) };
	writePacket(self, PROTOCOL_CONNECT, destination, 6);
	self->lastInAct = self->lastOutAct = HAL_GetTick();
	while (self->state != STATE_CONNECTED)
	{
		uint32_t now = HAL_GetTick();
		if (now - self->lastInAct >= 5000)
		{
			return -1;
		}
	}
	self->lastInAct = HAL_GetTick();
	return 1;
	/*	SUCCESS 1
	 TIMED_OUT -1
	 INVALID_SERVER -2
	 TRUNCATED -3
	 INVALID_RESPONSE -4
	 */
}

static uint8_t connectedPublic(const void* c)
{
	Client* self = (Client*)c;

	if (self->state == STATE_CONNECTED)
	{
		return 1;
	}
	return 0;
}

static void flushPublic(const void* c)
{
	Client* self = (Client*)c;

	while (!self->txReady);
}

static void stopPublic(const void* c)
{
	;
}

static size_t writePublic(const void* c, uint8_t* payload, uint8_t pLength)
{
	Client* self = (Client*)c;

	return publish(self, payload, pLength);
}

// Constructor
void newClient(Client* c, UART_HandleTypeDef* uartUnit, CRC_HandleTypeDef* crcUnit)
{
	c->peripheral_UART = uartUnit;
	c->peripheral_CRC = crcUnit;

	c->pHead_tx = 0;
	c->pTail_tx = 0;
	c->txFull = false;
	c->pRx_read = 0;
	c->pRead_read = 0;
	c->readFull = false;
	c->txReady = true;
	c->ackOutstanding = false;
	c->sequenceTxFlag = false;
	c->expectedRxSeqFlag = false;

	c->state = STATE_DISCONNECTED;

	// Arduino Client interface API
	c->connect = connectPublic;
	c->connected = connectedPublic;
	c->available = availablePublic;
	c->read = readPublic;
	c->write = writePublic;
	c->flush = flushPublic;
	c->stop = stopPublic;
}
