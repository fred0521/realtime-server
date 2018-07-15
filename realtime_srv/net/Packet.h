#pragma once

#include <memory>

#include <concurrent_queue/concurrentqueue.h>
#include <concurrent_queue/blockingconcurrentqueue.h>

#include <muduo_udp_support/UdpConnection.h>

#include "realtime_srv/rep/BitStream.h"


namespace realtime_srv
{

class ReceivedPacket
{
public:
	ReceivedPacket() {}
	ReceivedPacket(
		const float inReceivedTime,
		const std::shared_ptr<InputBitStream>& inInputMemoryBitStreamPtr,
		const muduo::net::UdpConnectionPtr& inUdpConnetction )
		:
		recvedTime_( inReceivedTime ),
		recvedPacketBuf_( inInputMemoryBitStreamPtr ),
		udpConn_( inUdpConnetction )
	{}
	muduo::net::UdpConnectionPtr&	GetUdpConn() { return udpConn_; }
	float GetReceivedTime()	const { return recvedTime_; }
	std::shared_ptr<InputBitStream>& GetPacketBuffer() { return recvedPacketBuf_; }

	bool operator<( const ReceivedPacket& other ) const
	{ return this->recvedTime_ < other.GetReceivedTime(); }

private:
	float																recvedTime_;
	std::shared_ptr<InputBitStream>			recvedPacketBuf_;
	muduo::net::UdpConnectionPtr				udpConn_;
};
typedef moodycamel::ConcurrentQueue<ReceivedPacket> ReceivedPacketQueue;
typedef moodycamel::BlockingConcurrentQueue<ReceivedPacket> ReceivedPacketBlockQueue;


class PendingSendPacket
{
public:
	PendingSendPacket() {}
	PendingSendPacket( std::shared_ptr<OutputBitStream>& OutPutPacketBuffer,
		muduo::net::UdpConnectionPtr& UdpConnection )
		:
		sndPacketBuf_( OutPutPacketBuffer ),
		udpConn_( UdpConnection )
	{}
	muduo::net::UdpConnectionPtr& GetUdpConnection() { return udpConn_; }
	std::shared_ptr<OutputBitStream>& GetPacketBuffer() { return sndPacketBuf_; }
private:
	std::shared_ptr<OutputBitStream>					sndPacketBuf_;
	muduo::net::UdpConnectionPtr							udpConn_;
};
typedef moodycamel::ConcurrentQueue< PendingSendPacket > PendingSendPacketQueue;
typedef moodycamel::BlockingConcurrentQueue< PendingSendPacket > PendingSendPacketBlockQueue;

}