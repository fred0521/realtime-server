#include "realtime_srv/common/RealtimeSrvShared.h"


using namespace realtime_srv;


#ifdef IS_LINUX

namespace
{
const float		kClientDisconnectTimeout = 6.f;
const size_t	kMaxPacketsPerFrameCount = 10;
}

using namespace muduo;
using namespace muduo::net;

AtomicInt32 NetworkMgr::kNewNetId;

NetworkMgr::NetworkMgr() :
	pktHandler_( &recvedPacketBlockQ_, std::bind( &NetworkMgr::PktProcessFunc, this, _1 ) )
{
	kNewNetId.getAndSet( 1 );
}

void NetworkMgr::PrepareGamePacket( ClientProxyPtr inClientProxy, const uint32_t inConnFlag )
{
	shared_ptr< OutputBitStream > outputPacket( new OutputBitStream() );
	outputPacket->Write( inConnFlag );

	InFlightPacket* ifp = inClientProxy->GetDeliveryNotifyManager()
		.WriteState( *outputPacket, inClientProxy.get() );

	switch ( inConnFlag )
	{
		case kResetCC:
		case kWelcomeCC:
			outputPacket->Write( inClientProxy->GetPlayerId() );
			outputPacket->Write( PktDispatcher::kSendPacketInterval );
			break;
		case kStateCC:
			WriteLastMoveTimestampIfDirty( *outputPacket, inClientProxy );
		default:
			break;
	}
	inClientProxy->GetReplicationManager().Write( *outputPacket, ifp );

	// test
	pendingSndPacketQ_.enqueue( PendingSendPacket(
		outputPacket, inClientProxy->GetUdpConnection() ) );
}

void NetworkMgr::PktProcessFunc( ReceivedPacket& recvedPacket )
{
	ProcessPacket( *( recvedPacket.GetPacketBuffer() ), recvedPacket.GetUdpConn() );
	worldUpdateCb_();
	PrepareOutgoingPackets();
}

void NetworkMgr::CheckForDisconnects()
{
	float minAllowedTime =
		RealtimeSrvTiming::sInst.GetCurrentGameTime() - kClientDisconnectTimeout;

	vector< ClientProxyPtr > clientsToDisconnect;

	for ( const auto& pair : udpConnToClientMap_ )
	{
		if ( pair.second->GetLastPacketFromClientTime() < minAllowedTime )
		{
			clientsToDisconnect.push_back( pair.second );
		}
	}
	if ( clientsToDisconnect.size() > 0 )
	{
		for ( auto cliToDC : clientsToDisconnect )
		{
			cliToDC->GetUdpConnection()->forceClose();
		}
	}
}

bool NetworkMgr::Init( uint16_t inPort )
{
	pktDispatcher_.Init( inPort,
		&recvedPacketBlockQ_, &pendingSndPacketQ_ );

	std::function< void() > checkDisconnCb =
		std::bind( &NetworkMgr::CheckForDisconnects, this );
	pktDispatcher_.SetInterval(
		std::bind( &PktHandler::AppendToPendingFuncs, &pktHandler_, checkDisconnCb ),
		static_cast< double >( kClientDisconnectTimeout ) );

	pktDispatcher_.SetConnCallback(
		std::bind( &NetworkMgr::OnConnOrDisconn, this, _1 ) );

	return true;
}

void NetworkMgr::OnConnOrDisconn( const UdpConnectionPtr& conn )
{
	if ( !conn->connected() )
	{
		pktHandler_.AppendToPendingFuncs(
			std::bind( &NetworkMgr::RemoveUdpConn, this, conn ) );
	}
}

void NetworkMgr::Start()
{
	pktHandler_.Start();
	pktDispatcher_.Start();
}

void NetworkMgr::RemoveUdpConn( const UdpConnectionPtr& conn )
{
	LOG( "Player %d disconnect", ( ( udpConnToClientMap_ )[conn] )->GetPlayerId() );
	udpConnToClientMap_.erase( conn );
}

void NetworkMgr::ProcessPacket( InputBitStream& inInputStream,
	const UdpConnectionPtr& inUdpConnetction )
{
	auto it = udpConnToClientMap_.find( inUdpConnetction );
	if ( it == udpConnToClientMap_.end() )
	{
		HandlePacketFromNewClient( inInputStream, inUdpConnetction );
	}
	else
	{
		DoProcessPacket( ( *it ).second, inInputStream );
	}
}

void NetworkMgr::HandlePacketFromNewClient( InputBitStream& inInputStream,
	const UdpConnectionPtr& inUdpConnetction )
{
	uint32_t	packetType;
	inInputStream.Read( packetType );
	if ( packetType == kHelloCC
		|| packetType == kInputCC
		|| packetType == kResetedCC )
	{
		std::string playerName = "realtime_srv_test_player_name";
		//inInputStream.Read( playerName );	

		ClientProxyPtr newClientProxy =
			std::make_shared< ClientProxy >(
				this,
				playerName,
				kNewNetId.getAndAdd( 1 ),
				inUdpConnetction );

		( udpConnToClientMap_ )[inUdpConnetction] = newClientProxy;

		GameObjPtr newGameObj = newPlayerCb_( newClientProxy );
		newGameObj->SetClientProxy( newClientProxy );
		worldRegistryCb_( newGameObj, RA_Create );

		if ( packetType == kHelloCC )
		{
			PrepareGamePacket( newClientProxy, kWelcomeCC );
		}
		else
		{
	 // Server reset
			newClientProxy->SetRecvingServerResetFlag( true );
			PrepareGamePacket( newClientProxy, kResetCC );
			LOG( "SendResetPacket" );
		}

		LOG( "a new client named '%s' as PlayerID %d ",
			newClientProxy->GetPlayerName().c_str(),
			newClientProxy->GetPlayerId() );
	}
	//else if ( packetType == kInputCC )
	//{
	//	// Server reset
	//	SendResetPacket( inFromAddress );
	//}
	else
	{
		LOG_INFO << "Bad incoming packet from unknown client at socket "
			<< inUdpConnetction->peerAddress().toIpPort() << " is "
			<< " - we're under attack!!";
	}
}

void NetworkMgr::NotifyAllClient( GameObjPtr inGameObject, ReplicationAction inAction )
{
	for ( const auto& pair : udpConnToClientMap_ )
	{
		switch ( inAction )
		{
			case RA_Create:
				pair.second->GetReplicationManager().ReplicateCreate(
					inGameObject->GetObjId(), inGameObject->GetAllStateMask() );
				break;
			case RA_Destroy:
				pair.second->GetReplicationManager().ReplicateDestroy(
					inGameObject->GetObjId() );
				break;
			default:
				break;
		}
	}
}

void NetworkMgr::PrepareOutgoingPackets()
{
	for ( const auto& pair : udpConnToClientMap_ )
	{
		( pair.second )->GetDeliveryNotifyManager().ProcessTimedOutPackets();

		if ( ( pair.second )->IsLastMoveTimestampDirty() )
		{
			PrepareGamePacket( ( pair.second ), kStateCC );
		}
	}
}

void NetworkMgr::SetRepStateDirty( int inNetworkId, uint32_t inDirtyState )
{
	for ( const auto& pair : udpConnToClientMap_ )
	{
		pair.second->GetReplicationManager().SetReplicationStateDirty(
			inNetworkId, inDirtyState );
	}
}

#else

namespace
{
const float kSendPacketInterval = 0.033f;
const float kClientDisconnectTimeout = 6.f;
const int		kMaxPacketsPerFrameCount = 10;
}

int NetworkMgr::kNewNetId = 1;

NetworkMgr::NetworkMgr() :
	mTimeOfLastStatePacket( 0.f ),
	mLastCheckDCTime( 0.f ),
	mDropPacketChance( 0.f ),
	mSimulatedLatency( 0.f ),
	mSimulateJitter( false ),
	isSimilateRealWorld_( false )
{}

void NetworkMgr::Start()
{
	while ( true )
	{
		ReadIncomingPacketsIntoQueue();
		ProcessQueuedPackets();
		CheckForDisconnects();
		worldUpdateCb_();
		SendOutgoingPackets();
	}
}

void NetworkMgr::ReadIncomingPacketsIntoQueue()
{
	char packetMem[MAX_PACKET_BYTE_LENGTH];
	int packetSize = sizeof( packetMem );
	SockAddrInterf fromAddress;

	int receivedPackedCount = 0;
	int totalReadByteCount = 0;

	while ( receivedPackedCount < kMaxPacketsPerFrameCount )
	{
		int readByteCount = mSocket->ReceiveFrom( packetMem, packetSize, fromAddress );
		if ( readByteCount == 0 )
		{
//LOG( "ReadIncomingPacketsIntoQueue readByteCount = %d " );
			break;
		}
		else if ( readByteCount == -WSAECONNRESET )
		{
			HandleConnectionReset( fromAddress );
		}
		else if ( readByteCount > 0 )
		{
			InputBitStream inputStream( packetMem, readByteCount * 8 );
			++receivedPackedCount;
			totalReadByteCount += readByteCount;

			if ( RealtimeSrvMath::GetRandomFloat() >= mDropPacketChance )
			{
				float simulatedReceivedTime =
					RealtimeSrvTiming::sInst.GetCurrentGameTime() +
					mSimulatedLatency +
					( mSimulateJitter ?
						RealtimeSrvMath::Clamp( RealtimeSrvMath::GetRandomFloat(),
							0.f, mSimulatedLatency ) : 0.f );
				mPacketQueue.emplace( simulatedReceivedTime, inputStream, fromAddress );
			}
			else
			{
			 //LOG( "Dropped packet!" );
			}
		}
		else
		{
		}
	}

	if ( totalReadByteCount > 0 )
	{
//mBytesReceivedPerSecond.UpdatePerSecond( static_cast< float >( totalReadByteCount ) );
	}
}

void NetworkMgr::ProcessQueuedPackets()
{
	while ( !mPacketQueue.empty() )
	{
		ReceivedPacket& nextPacket = mPacketQueue.front();
		if ( RealtimeSrvTiming::sInst.GetCurrentGameTime()
			> nextPacket.GetReceivedTime() )
		{
			ProcessPacket(
				nextPacket.GetPacketBuffer(),
				nextPacket.GetFromAddress(),
				nextPacket.GetUDPSocket()
			);
			mPacketQueue.pop();
		}
		else
		{
			break;
		}
	}
}

bool NetworkMgr::Init( uint16_t inPort, bool isLazy /*= false*/ )
{
	UdpSockInterf::StaticInit();

	mSocket = UdpSockInterf::CreateUDPSocket();
	if ( mSocket == nullptr )
	{
		return false;
	}
	SockAddrInterf ownAddress( INADDR_ANY, inPort );
	mSocket->Bind( ownAddress );
	if ( mSocket->SetNonBlockingMode( true ) != NO_ERROR )
	{
		return false;
	}

	LOG( "Initializing NetworkManager at port %d", inPort );
	return true;
}

void NetworkMgr::SendPacket( const OutputBitStream& inOutputStream,
	const SockAddrInterf& inSockAddr )
{
	if ( RealtimeSrvMath::GetRandomFloat() < mDropPacketChance )
	{
		return;
	}
	int sentByteCount = mSocket->SendTo( inOutputStream.GetBufferPtr(),
		inOutputStream.GetByteLength(),
		inSockAddr );
}

void NetworkMgr::HandleConnectionReset( const SockAddrInterf& inFromAddress )
{
	auto it = addrToClientMap_.find( inFromAddress );
	if ( it != addrToClientMap_.end() )
	{
		addrToClientMap_.erase( it );
	}
}

void NetworkMgr::ProcessPacket(
	InputBitStream& inInputStream,
	const SockAddrInterf& inFromAddress,
	const UDPSocketPtr& inUDPSocket
)
{
	auto it = addrToClientMap_.find( inFromAddress );
	if ( it == addrToClientMap_.end() )
	{
		HandlePacketFromNewClient( inInputStream,
			inFromAddress,
			inUDPSocket
		);
	}
	else
	{
		DoProcessPacket( ( *it ).second, inInputStream );
	}
}

void NetworkMgr::HandlePacketFromNewClient( InputBitStream& inInputStream,
	const SockAddrInterf& inFromAddress,
	const UDPSocketPtr& inUDPSocket )
{
	uint32_t	packetType;
	inInputStream.Read( packetType );
	if ( packetType == kHelloCC
		|| packetType == kInputCC
		|| packetType == kResetedCC
		)
	{
		std::string playerName = "rs_test_player_name";
		//inInputStream.Read( playerName );	

		ClientProxyPtr newClientProxy = std::make_shared< ClientProxy >(
			this,
			inFromAddress,
			playerName,
			kNewNetId++,
			inUDPSocket );

		addrToClientMap_[inFromAddress] = newClientProxy;

		GameObjPtr newGameObj = newPlayerCb_( newClientProxy );
		newGameObj->SetClientProxy( newClientProxy );
		worldRegistryCb_( newGameObj, RA_Create );

		if ( packetType == kHelloCC )
		{
			SendGamePacket( newClientProxy, kWelcomeCC );
		}
		else
		{
	 // Server reset
			newClientProxy->SetRecvingServerResetFlag( true );
			SendGamePacket( newClientProxy, kResetCC );
			LOG( "SendResetPacket" );
		}
		LOG( "a new client at socket %s, named '%s' as PlayerID %d ",
			inFromAddress.ToString().c_str(),
			newClientProxy->GetPlayerName().c_str(),
			newClientProxy->GetPlayerId()
		);
	}
	//else if ( packetType == kInputCC )
	//{
	//	// Server reset
	//	SendResetPacket( inFromAddress );
	//}
	else
	{
		LOG( "Bad incoming packet from unknown client at socket %s - we're under attack!!", inFromAddress.ToString().c_str() );
	}
}


void NetworkMgr::CheckForDisconnects()
{
	float curTime = RealtimeSrvTiming::sInst.GetCurrentGameTime();

	if ( curTime - mLastCheckDCTime < kClientDisconnectTimeout )
	{
		return;
	}
	mLastCheckDCTime = curTime;

	float minAllowedTime = RealtimeSrvTiming::sInst.GetCurrentGameTime()
		- kClientDisconnectTimeout;
	for ( AddrToClientMap::iterator it = addrToClientMap_.begin();
		it != addrToClientMap_.end(); )
	{
		if ( it->second->GetLastPacketFromClientTime() < minAllowedTime )
		{
			LOG( "Player %d disconnect", it->second->GetPlayerId() );

			it = addrToClientMap_.erase( it );
		}
		else
		{
			++it;
		}
	}
}

void NetworkMgr::SendOutgoingPackets()
{
	float time = RealtimeSrvTiming::sInst.GetCurrentGameTime();

	if ( time < mTimeOfLastStatePacket + kSendPacketInterval )
	{
		return;
	}
	mTimeOfLastStatePacket = time;

	for ( auto it = addrToClientMap_.begin(), end = addrToClientMap_.end();
		it != end; ++it )
	{
		ClientProxyPtr clientProxy = it->second;
		clientProxy->GetDeliveryNotifyManager().ProcessTimedOutPackets();
		if ( clientProxy->IsLastMoveTimestampDirty() )
		{
			//SendStatePacketToClient( clientProxy );
			SendGamePacket( clientProxy, kStateCC );
		}
	}
}

void NetworkMgr::SetRepStateDirty( int inNetworkId, uint32_t inDirtyState )
{
	for ( const auto& pair : addrToClientMap_ )
		pair.second->GetReplicationManager().SetReplicationStateDirty(
			inNetworkId, inDirtyState );
}

void NetworkMgr::NotifyAllClient( GameObjPtr inGameObject,
	ReplicationAction inAction )
{
	for ( const auto& pair : addrToClientMap_ )
	{
		switch ( inAction )
		{
			case RA_Create:
				pair.second->GetReplicationManager().ReplicateCreate(
					inGameObject->GetObjId(), inGameObject->GetAllStateMask() );
				break;
			case RA_Destroy:
				pair.second->GetReplicationManager().ReplicateDestroy(
					inGameObject->GetObjId() );
				break;
			default:
				break;
		}
	}
}

void NetworkMgr::SendGamePacket( ClientProxyPtr inClientProxy, const uint32_t inConnFlag )
{
	shared_ptr< OutputBitStream > outputPacket( new OutputBitStream() );
	outputPacket->Write( inConnFlag );

	InFlightPacket* ifp = inClientProxy->GetDeliveryNotifyManager()
		.WriteState( *outputPacket, inClientProxy.get() );

	switch ( inConnFlag )
	{
		case kResetCC:
		case kWelcomeCC:
			outputPacket->Write( inClientProxy->GetPlayerId() );
			outputPacket->Write( kSendPacketInterval );
			break;
		case kStateCC:
			WriteLastMoveTimestampIfDirty( *outputPacket, inClientProxy );
		default:
			break;
	}
	inClientProxy->GetReplicationManager().Write( *outputPacket, ifp );

	SendPacket( outputPacket, inClientProxy->GetSocketAddress() );
}
#endif //IS_LINUX


void NetworkMgr::DoProcessPacket( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	inClientProxy->UpdateLastPacketTime();

	uint32_t packetType = HandleServerReset( inClientProxy, inInputStream );
	switch ( packetType )
	{
		case kHelloCC:
		#ifdef IS_LINUX
			PrepareGamePacket( inClientProxy, kWelcomeCC );
		#else
			SendGamePacket( inClientProxy, kWelcomeCC );
		#endif //IS_LINUX
			break;
		case kInputCC:
			if ( inClientProxy->GetDeliveryNotifyManager().ReadAndProcessState( inInputStream ) )
			{
				HandleInputPacket( inClientProxy, inInputStream );
			}
			break;
		case kNullCC:
			break;
		default:
		#ifdef IS_LINUX
			LOG_INFO << "Unknown packet type received from "
				<< inClientProxy->GetUdpConnection()->peerAddress().toIpPort();
		#else
			LOG( "Unknown packet type received from %s", inClientProxy->GetSocketAddress().ToString().c_str() );
		#endif //IS_LINUX
			break;
	}
}

uint32_t NetworkMgr::HandleServerReset( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	uint32_t packetType;
	inInputStream.Read( packetType );

	if ( packetType == kResetedCC )
	{
		inClientProxy->SetRecvingServerResetFlag( false );
		inInputStream.Read( packetType );
	}
	if ( inClientProxy->GetRecvingServerResetFlag() == true )
	{
	#ifdef IS_LINUX
		PrepareGamePacket( inClientProxy, kWelcomeCC );
	#else
		SendGamePacket( inClientProxy, kWelcomeCC );
	#endif //IS_LINUX
		return kNullCC;
	}
	else
	{
		return packetType;
	}
}

void NetworkMgr::HandleInputPacket( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	uint32_t moveCount = 0;
	Action move;
	inInputStream.Read( moveCount, MOVE_COUNT_NUM );

	for ( ; moveCount > 0; --moveCount )
	{
		if ( move.Read( inInputStream ) )
		{
			if ( inClientProxy->GetUnprocessedActionList().AddMoveIfNew( move ) )
			{
				inClientProxy->SetIsLastMoveTimestampDirty( true );
			}
		}
	}
}

void NetworkMgr::WriteLastMoveTimestampIfDirty( OutputBitStream& inOutputStream,
	ClientProxyPtr inClientProxy )
{
	bool isTimestampDirty = inClientProxy->IsLastMoveTimestampDirty();
	inOutputStream.Write( isTimestampDirty );
	if ( isTimestampDirty )
	{
		inOutputStream.Write( inClientProxy->GetUnprocessedActionList()
			.GetLastMoveTimestamp() );
		inClientProxy->SetIsLastMoveTimestampDirty( false );
	}
}