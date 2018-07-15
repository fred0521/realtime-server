#pragma once

#include <memory>
#include <map>

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/Endian.h>

#include <muduo_udp_support/UdpServer.h>
#include <muduo_udp_support/UdpConnection.h>

#include <realtime_srv/net/Packet.h>



namespace realtime_srv
{

class PktHandler;

class PktDispatcher
{
public:
	typedef std::function<
		void( const muduo::net::UdpConnectionPtr& )
	> UdpConnectionCallback;

	static const float	kSendPacketInterval;

public:
	bool Init( uint16_t inPort,
		ReceivedPacketBlockQueue* const inRecvPktBQ,
		PendingSendPacketQueue* const inSndPktQ );

	void Start();

	void SetInterval( std::function<void()> func, double interval );

	void SetConnCallback( const UdpConnectionCallback& cb )
	{ connCb_ = cb; }

protected:
	void SendGamePacket();

	void IoThreadInit( muduo::net::EventLoop* loop );

	void onMessage( const muduo::net::UdpConnectionPtr& conn,
		muduo::net::Buffer* buf, muduo::Timestamp receiveTime );

	void onConnection( const muduo::net::UdpConnectionPtr& conn );

private:
	struct LoopAndTimerId
	{
		LoopAndTimerId() {}
		LoopAndTimerId(
			muduo::net::EventLoop* inLoop,
			muduo::net::TimerId		inTimerId )
			:
			loop_( inLoop ),
			timerId_( inTimerId )
		{}
		muduo::net::EventLoop* loop_;
		muduo::net::TimerId		timerId_;
	};
	std::map<int, LoopAndTimerId> tidToLoopAndTimerIdMap_;

	UdpConnectionCallback connCb_;
	ReceivedPacketBlockQueue* recvedPktBQ_;
	PendingSendPacketQueue* pendingSndPktQ_;

	muduo::net::EventLoop serverBaseLoop_;
	std::unique_ptr<muduo::net::UdpServer> server_;
};

}