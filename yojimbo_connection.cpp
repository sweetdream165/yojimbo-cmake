/*
    Yojimbo Network Library.
    
    Copyright © 2016 - 2017, The Network Protocol Company, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "yojimbo_config.h"
#include "yojimbo_connection.h"

namespace yojimbo
{
    /** 
        This packet carries messages sent across connection channels.

        Connection packets should be generated and sent at a steady rate like 10, 20 or 30 times per-second in both directions across a connection. 
     */

    struct ConnectionPacket
    {
        int numChannelEntries;                                                  ///< The number of channel entries in this packet.
        ChannelPacketData * channelEntry;                                       ///< Per-channel message data that was included in this packet.
        MessageFactory * messageFactory;                                        ///< The message factory is cached so we can release messages included in this packet when it is destroyed.

        /**
            Connection packet constructor.
         */

        ConnectionPacket();

        /** 
            Connection packet destructor.

            Releases all references to messages included in this packet.

            @see Message
            @see MessageFactory
            @see ChannelPacketData
         */

        ~ConnectionPacket();

        /** 
            Allocate channel data in this packet.

            The allocation is performed with the allocator that is set on the message factory.

            When this is used on the server, the allocator corresponds to the per-client allocator corresponding to the client that is sending this connection packet. See Server::m_clientAllocator.

            This is intended to silo each client to their own set of resources on the server, so malicious clients cannot launch an attack to deplete resources shared with other clients.

            @param messageFactory The message factory used to create and destroy messages.
            @param numEntries The number of channel entries to allocate. This corresponds to the number of channels that have data to include in the connection packet.

            @returns True if the allocation succeeded, false otherwise.
         */

        bool AllocateChannelData( MessageFactory & messageFactory, int numEntries );

        /** 
            The template function for serializing the connection packet.

            Unifies packet read and write, making it harder to accidentally desync one from the other.
         */

        template <typename Stream> bool Serialize( Stream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig );

        /// Implements serialize read by calling into ConnectionPacket::Serialize with a ReadStream.

        bool SerializeInternal( ReadStream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig );

        /// Implements serialize write by calling into ConnectionPacket::Serialize with a WriteStream.

        bool SerializeInternal( WriteStream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig );

        /// Implements serialize measure by calling into ConnectionPacket::Serialize with a MeasureStream.

        bool SerializeInternal( MeasureStream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig );

    private:

        ConnectionPacket( const ConnectionPacket & other );

        const ConnectionPacket & operator = ( const ConnectionPacket & other );
    };

    ConnectionPacket::ConnectionPacket()
    {
        messageFactory = NULL;
        numChannelEntries = 0;
        channelEntry = NULL;
    }

    ConnectionPacket::~ConnectionPacket()
    {
        if ( messageFactory )
        {
            // todo: shouldn't we be cleaning up channel entries here?
        }
    }

    bool ConnectionPacket::AllocateChannelData( MessageFactory & messageFactory, int numEntries )
    {
        assert( numEntries > 0 );
        assert( numEntries <= MaxChannels );
        this->messageFactory = &messageFactory;
        Allocator & allocator = messageFactory.GetAllocator();
        channelEntry = (ChannelPacketData*) YOJIMBO_ALLOCATE( allocator, sizeof( ChannelPacketData ) * numEntries );
        if ( channelEntry == NULL )
            return false;
        for ( int i = 0; i < numEntries; ++i )
        {
            channelEntry[i].Initialize();
        }
        numChannelEntries = numEntries;
        return true;
    }

    template <typename Stream> bool ConnectionPacket::Serialize( Stream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig )
    {
        const int numChannels = connectionConfig.numChannels;
        serialize_int( stream, numChannelEntries, 0, connectionConfig.numChannels );
#if YOJIMBO_DEBUG_MESSAGE_BUDGET
        assert( stream.GetBitsProcessed() <= ConservativeConnectionPacketHeaderEstimate );
#endif // #if YOJIMBO_DEBUG_MESSAGE_BUDGET
        if ( numChannelEntries > 0 )
        {
            if ( Stream::IsReading )
            {
                if ( !AllocateChannelData( messageFactory, numChannelEntries ) )
                {
                    debug_printf( "error: failed to allocate channel data (ConnectionPacket)\n" );
                    return false;
                }
                for ( int i = 0; i < numChannelEntries; ++i )
                {
                    assert( channelEntry[i].messageFailedToSerialize == 0 );
                }
            }
            for ( int i = 0; i < numChannelEntries; ++i )
            {
                assert( channelEntry[i].messageFailedToSerialize == 0 );
                if ( !channelEntry[i].SerializeInternal( stream, messageFactory, connectionConfig.channel, numChannels ) )
                {
                    debug_printf( "error: failed to serialize channel %d\n", i );
                    return false;
                }
            }
        }
        return true;
    }

    bool ConnectionPacket::SerializeInternal( ReadStream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig )
    {
        return Serialize( stream, messageFactory, connectionConfig );
    }

    bool ConnectionPacket::SerializeInternal( WriteStream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig )
    {
        return Serialize( stream, messageFactory, connectionConfig );
    }

    bool ConnectionPacket::SerializeInternal( MeasureStream & stream, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig )
    {
        return Serialize( stream, messageFactory, connectionConfig );
    }

    // ------------------------------------------------------------------------------

    Connection::Connection( Allocator & allocator, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig )
    {
        m_allocator = &allocator;
        m_messageFactory = &messageFactory;
        m_connectionConfig = connectionConfig;
        memset( m_channel, 0, sizeof( m_channel ) );
        assert( m_connectionConfig.numChannels >= 1 );
        assert( m_connectionConfig.numChannels <= MaxChannels );
        for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
        {
            switch ( m_connectionConfig.channel[channelId].type )
            {
                case CHANNEL_TYPE_RELIABLE_ORDERED: 
                    m_channel[channelId] = YOJIMBO_NEW( *m_allocator, ReliableOrderedChannel, *m_allocator, messageFactory, m_connectionConfig.channel[channelId], channelId ); 
                    break;

                case CHANNEL_TYPE_UNRELIABLE_UNORDERED: 
                    m_channel[channelId] = YOJIMBO_NEW( *m_allocator, UnreliableUnorderedChannel, *m_allocator, messageFactory, m_connectionConfig.channel[channelId], channelId ); 
                    break;
                // todo: unreliable ordered channel
                default: 
                    assert( !"unknown channel type" );
            }
        }
    }

    Connection::~Connection()
    {
        assert( m_allocator );
        Reset();
        for ( int i = 0; i < m_connectionConfig.numChannels; ++i )
        {
            YOJIMBO_DELETE( *m_allocator, Channel, m_channel[i] );
        }
        m_allocator = NULL;
    }

    void Connection::Reset()
    {
        // todo
        //m_error = CONNECTION_ERROR_NONE;
        for ( int i = 0; i < m_connectionConfig.numChannels; ++i )
            m_channel[i]->Reset();
    }

    static int WritePacket( Allocator & allocator, void * context, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig, ConnectionPacket & packet, uint8_t * buffer, int bufferSize )
    {
        WriteStream stream( buffer, bufferSize, allocator );

        stream.SetContext( context );

        if ( !packet.SerializeInternal( stream, messageFactory, connectionConfig ) )
        {
            debug_printf( "serialize connection packet failed (write packet)\n" );
            return 0;
        }

        if ( !stream.SerializeCheck() )
        {
            debug_printf( "serialize check at end of connection packed failed (write packet)\n" );
            return 0;
        }

        stream.Flush();

        return stream.GetBytesProcessed();
    }

    bool Connection::GeneratePacket( void * context, uint16_t packetSequence, uint8_t * packetData, int maxPacketBytes, int & packetBytes )
    {
        (void) context;

        ConnectionPacket packet;

        if ( m_connectionConfig.numChannels > 0 )
        {
            int numChannelsWithData = 0;
            bool channelHasData[MaxChannels];
            memset( channelHasData, 0, sizeof( channelHasData ) );
            ChannelPacketData channelData[MaxChannels];

            int availableBits = maxPacketBytes * 8;

            availableBits -= ConservativeConnectionPacketHeaderEstimate;

            for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
            {
                int packetDataBits = m_channel[channelId]->GetPacketData( channelData[channelId], packetSequence, availableBits );

                if ( packetDataBits > 0 )
                {
                    availableBits -= ConservativeChannelHeaderEstimate;

                    availableBits -= packetDataBits;

                    channelHasData[channelId] = true;

                    numChannelsWithData++;
                }
            }

            if ( numChannelsWithData > 0 )
            {
                if ( !packet.AllocateChannelData( *m_messageFactory, numChannelsWithData ) )
                {
                    // todo: bring back connection errors
                    //m_error = CONNECTION_ERROR_OUT_OF_MEMORY;
                    return false;
                }

                int index = 0;

                for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
                {
                    if ( channelHasData[channelId] )
                    {
                        memcpy( &packet.channelEntry[index], &channelData[channelId], sizeof( ChannelPacketData ) );
                        index++;
                    }
                }
            }
        }

        packetBytes = WritePacket( m_messageFactory->GetAllocator(), context, *m_messageFactory, m_connectionConfig, packet, packetData, maxPacketBytes );

        return true;
    }

    bool Connection::ProcessPacket( void * context, uint16_t packetSequence, const uint8_t * packetData, int packetBytes )
    {
        (void) context;
        (void) packetSequence;
        (void) packetData;
        (void) packetBytes;
        // todo: deserialize packet
        // todo: pass channel data to each channel in turn for processing
        return true;
    }

    void Connection::ProcessAcks( const uint16_t * acks, int numAcks )
    {
        for ( int i = 0; i < numAcks; ++i )
        {
            for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
            {
                m_channel[channelId]->ProcessAck( acks[i] );
            }
        }
    }

    void Connection::AdvanceTime( double time )
    {
        for ( int i = 0; i < m_connectionConfig.numChannels; ++i )
        {
            m_channel[i]->AdvanceTime( time );
            // todo: channel error
            /*
            ChannelError error = m_channel[i]->GetError();

            if ( error != CHANNEL_ERROR_NONE )
            {
                m_error = CONNECTION_ERROR_CHANNEL;
                return;
            }
            */
        }
    }
}














#if 0 // old stuff

namespace yojimbo
{

    Connection::Connection( Allocator & allocator, PacketFactory & packetFactory, MessageFactory & messageFactory, const ConnectionConfig & connectionConfig ) : m_connectionConfig( connectionConfig )
    {
        assert( ( 65536 % connectionConfig.slidingWindowSize ) == 0 );

        m_allocator = &allocator;

        m_packetFactory = &packetFactory;

        m_messageFactory = &messageFactory;

        m_listener = NULL;
        
        m_error = CONNECTION_ERROR_NONE;

        m_clientIndex = 0;

        memset( m_channel, 0, sizeof( m_channel ) );

        assert( m_connectionConfig.numChannels >= 1 );
        assert( m_connectionConfig.numChannels <= MaxChannels );

        for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
        {
            switch ( m_connectionConfig.channel[channelId].type )
            {
                case CHANNEL_TYPE_RELIABLE_ORDERED: 
                    m_channel[channelId] = YOJIMBO_NEW( *m_allocator, ReliableOrderedChannel, *m_allocator, messageFactory, m_connectionConfig.channel[channelId], channelId ); 
                    break;

                case CHANNEL_TYPE_UNRELIABLE_UNORDERED: 
                    m_channel[channelId] = YOJIMBO_NEW( *m_allocator, UnreliableUnorderedChannel, *m_allocator, messageFactory, m_connectionConfig.channel[channelId], channelId ); 
                    break;

                default: 
                    assert( !"unknown channel type" );
            }

            m_channel[channelId]->SetListener( this );
        }

        m_sentPackets = YOJIMBO_NEW( *m_allocator, SequenceBuffer<ConnectionSentPacketData>, *m_allocator, m_connectionConfig.slidingWindowSize );
        
        m_receivedPackets = YOJIMBO_NEW( *m_allocator, SequenceBuffer<ConnectionReceivedPacketData>, *m_allocator, m_connectionConfig.slidingWindowSize );

        Reset();
    }

    Connection::~Connection()
    {
        Reset();

        for ( int i = 0; i < m_connectionConfig.numChannels; ++i )
            YOJIMBO_DELETE( *m_allocator, Channel, m_channel[i] );

        YOJIMBO_DELETE( *m_allocator, SequenceBuffer<ConnectionSentPacketData>, m_sentPackets );
        YOJIMBO_DELETE( *m_allocator, SequenceBuffer<ConnectionReceivedPacketData>, m_receivedPackets );
    }

    void Connection::Reset()
    {
        m_error = CONNECTION_ERROR_NONE;

        for ( int i = 0; i < m_connectionConfig.numChannels; ++i )
            m_channel[i]->Reset();

        m_sentPackets->Reset();
        m_receivedPackets->Reset();

        memset( m_counters, 0, sizeof( m_counters ) );
    }

    bool Connection::CanSendMsg( int channelId ) const
    {
        assert( channelId >= 0 );
        assert( channelId < m_connectionConfig.numChannels );
        return m_channel[channelId]->CanSendMsg();
    }

    void Connection::SendMsg( Message * message, int channelId )
    {
        assert( channelId >= 0 );
        assert( channelId < m_connectionConfig.numChannels );
        return m_channel[channelId]->SendMsg( message );
    }

    Message * Connection::ReceiveMsg( int channelId )
    {
        assert( channelId >= 0 );
        assert( channelId < m_connectionConfig.numChannels );
        return m_channel[channelId]->ReceiveMsg();
    }

    ConnectionPacket * Connection::GeneratePacket()
    {
        if ( m_error != CONNECTION_ERROR_NONE )
            return NULL;

        ConnectionPacket * packet = (ConnectionPacket*) m_packetFactory->Create( m_connectionConfig.connectionPacketType );

        if ( !packet )
            return NULL;

        packet->sequence = m_sentPackets->GetSequence();

        GenerateAckBits( *m_receivedPackets, packet->ack, packet->ack_bits );

        InsertAckPacketEntry( packet->sequence );

        if ( m_connectionConfig.numChannels > 0 )
        {
            int numChannelsWithData = 0;
            bool channelHasData[MaxChannels];
            memset( channelHasData, 0, sizeof( channelHasData ) );
            ChannelPacketData channelData[MaxChannels];

            int availableBits = m_connectionConfig.maxPacketSize * 8;

            availableBits -= ConservativeConnectionPacketHeaderEstimate;

            for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
            {
                int packetDataBits = m_channel[channelId]->GetPacketData( channelData[channelId], packet->sequence, availableBits );

                if ( packetDataBits > 0 )
                {
                    availableBits -= ConservativeChannelHeaderEstimate;

                    availableBits -= packetDataBits;

                    channelHasData[channelId] = true;

                    numChannelsWithData++;
                }
            }

            if ( numChannelsWithData > 0 )
            {
                if ( !packet->AllocateChannelData( *m_messageFactory, numChannelsWithData ) )
                {
                    m_error = CONNECTION_ERROR_OUT_OF_MEMORY;
                    return NULL;
                }

                int index = 0;

                for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
                {
                    if ( channelHasData[channelId] )
                    {
                        memcpy( &packet->channelEntry[index], &channelData[channelId], sizeof( ChannelPacketData ) );
                        index++;
                    }
                }
            }
        }

        m_counters[CONNECTION_COUNTER_PACKETS_GENERATED]++;

        if ( m_listener )
            m_listener->OnConnectionPacketGenerated( this, packet->sequence );

        return packet;
    }

    bool Connection::ProcessPacket( ConnectionPacket * packet )
    {
        if ( m_error != CONNECTION_ERROR_NONE )
            return false;

        assert( packet );
        assert( packet->GetType() == m_connectionConfig.connectionPacketType );

        if ( !m_receivedPackets->Insert( packet->sequence ) )
        {
            m_counters[CONNECTION_COUNTER_PACKETS_STALE]++;
            return false;
        }

        m_counters[CONNECTION_COUNTER_PACKETS_PROCESSED]++;

        if ( m_listener )
            m_listener->OnConnectionPacketReceived( this, packet->sequence );

        ProcessAcks( packet->ack, packet->ack_bits );

        for ( int i = 0; i < packet->numChannelEntries; ++i )
        {
            const int channelId = packet->channelEntry[i].channelId;

            assert( channelId >= 0 );
            assert( channelId <= m_connectionConfig.numChannels );

            m_channel[channelId]->ProcessPacketData( packet->channelEntry[i], packet->sequence );
        }

        return true;
    }

    void Connection::AdvanceTime( double time )
    {
        for ( int i = 0; i < m_connectionConfig.numChannels; ++i )
        {
            m_channel[i]->AdvanceTime( time );

            ChannelError error = m_channel[i]->GetError();

            if ( error != CHANNEL_ERROR_NONE )
            {
                m_error = CONNECTION_ERROR_CHANNEL;
                return;
            }
        }
    }
    
    uint64_t Connection::GetCounter( int index ) const
    {
        assert( index >= 0 );
        assert( index < CONNECTION_COUNTER_NUM_COUNTERS );
        return m_counters[index];
    }

    ConnectionError Connection::GetError() const
    {
        return m_error;
    }

    void Connection::InsertAckPacketEntry( uint16_t sequence )
    {
        ConnectionSentPacketData * entry = m_sentPackets->Insert( sequence );
        
        assert( entry );

        if ( entry )
        {
            entry->acked = 0;
        }
    }

    void Connection::ProcessAcks( uint16_t ack, uint32_t ack_bits )
    {
        for ( int i = 0; i < 32; ++i )
        {
            if ( ack_bits & 1 )
            {                    
                const uint16_t sequence = ack - i;
                ConnectionSentPacketData * packetData = m_sentPackets->Find( sequence );
                if ( packetData && !packetData->acked )
                {
                    PacketAcked( sequence );
                    packetData->acked = 1;
                }
            }
            ack_bits >>= 1;
        }
    }

    void Connection::PacketAcked( uint16_t sequence )
    {
        OnPacketAcked( sequence );

        for ( int channelId = 0; channelId < m_connectionConfig.numChannels; ++channelId )
            m_channel[channelId]->ProcessAck( sequence );

        m_counters[CONNECTION_COUNTER_PACKETS_ACKED]++;
    }

    void Connection::OnPacketAcked( uint16_t sequence )
    {
        if ( m_listener )
        {
            m_listener->OnConnectionPacketAcked( this, sequence );
        }
    }

    void Connection::OnChannelFragmentReceived( class Channel * channel, uint16_t messageId, uint16_t fragmentId, int fragmentBytes, int numFragmentsReceived, int numFragmentsInBlock )
    {
        if ( m_listener )
        {
            m_listener->OnConnectionFragmentReceived( this, channel->GetChannelId(), messageId, fragmentId, fragmentBytes, numFragmentsReceived, numFragmentsInBlock );
        }
    }
}

#endif
