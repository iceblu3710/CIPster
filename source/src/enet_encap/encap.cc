/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * Copyright (C) 2016, SoftPLC Corporation.
 *
 ******************************************************************************/

#include "encap.h"

#include <string.h>
#include <stdlib.h>

#include <cipster_api.h>
#include <byte_bufs.h>


#include "../cip/cipcommon.h"
#include "../cip/cipmessagerouter.h"
#include "../cip/cipconnectionmanager.h"
#include "../cip/cipidentity.h"
#include "../cip/ciptcpipinterface.h"


/// Capability flags
enum CapabilityFlags
{
    kCapabilityFlagsCipTcp = 0x0020,
    kCapabilityFlagsCipUdpClass0or1 = 0x0100
};



//-----<DelayedMsg>-------------------------------------------------------------

/// According to EIP spec at least 2 delayed message requests should be supported
#define ENCAP_NUMBER_OF_SUPPORTED_DELAYED_ENCAP_MESSAGES    2

// currently we only have the size of an encapsulation message
#define ENCAP_MAX_DELAYED_ENCAP_MESSAGE_SIZE   \
    ( ENCAPSULATION_HEADER_LENGTH + 39 + sizeof(CIPSTER_DEVICE_NAME) )


struct DelayedMsg
{
    DelayedMsg() :
        socket( -1 ),
        message_size( 0 )
    {}

    EipInt32    time_out_usecs;     // must be signed 32 bits
    int         socket;
    SockAddr    receiver;
    EipByte     message[ENCAP_MAX_DELAYED_ENCAP_MESSAGE_SIZE];
    unsigned    message_size;

    BufReader Payload() const
    {
        return BufReader( message, message_size );
    }

    static DelayedMsg messages[ENCAP_NUMBER_OF_SUPPORTED_DELAYED_ENCAP_MESSAGES];
};

DelayedMsg DelayedMsg::messages[ENCAP_NUMBER_OF_SUPPORTED_DELAYED_ENCAP_MESSAGES];


//-----<ServerSessionMgr>-------------------------------------------------------

EncapSession ServerSessionMgr::sessions[CIPSTER_NUMBER_OF_SUPPORTED_SESSIONS];


void ServerSessionMgr::Init()
{
    /* now done in static constructor

    initialize Sessions to invalid == free session
    for( int i = 0; i < DIM(sessions); ++i )
    {
        sessions[i].m_socket = kSocketInvalid;
    }
    */
}


EncapError ServerSessionMgr::RegisterTcpConnection( int aSocket )
{
    int index;

    for( index = 0; index < DIM(sessions); ++index )
    {
        if( sessions[index].m_socket == kSocketInvalid )
            break;
    }

    if( index == DIM(sessions) )
    {
        return kEncapErrorInsufficientMemory;
    }

    EncapSession& ses = sessions[index];

    ses.m_socket = aSocket;

    // Fetch IP address of other end of this TCP connection and save
    // in Session::sockaddr.

    socklen_t   addrz = SADDRZ;

    // we have a peer to peer producer or a consuming connection
    int rc = getpeername( aSocket, ses.m_peeraddr, &addrz );

    if( rc < 0 )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: could not get peername: %s\n",
                __func__, aSocket, strerrno().c_str() );
        return kEncapErrorIncorrectData;
    }
    else
    {
        CIPSTER_TRACE_INFO( "%s[%d]: session peer:%s\n",
            __func__, aSocket, ses.m_peeraddr.AddrStr().c_str() );
    }

    ses.m_last_activity_usecs = g_current_usecs;    // last activity

    return kEncapErrorSuccess;
}


EncapError ServerSessionMgr::RegisterSession(
        int aSocket, CipUdint* aSessionHandleResult )
{
    int index;

    for( index = 0; index < DIM(sessions); ++index )
    {
        if( sessions[index].m_socket == aSocket )
            break;
    }

    // A bug because any TCP socket should be in sessions[] as unregistered by now
    CIPSTER_ASSERT( index < DIM(sessions) );

    if( index == DIM(sessions) )
    {
        // should never happen in Debug build because of ASSERT above
        return kEncapErrorInsufficientMemory;
    }

    EncapSession& ses = sessions[index];

    if( ses.m_is_registered )
    {
        // The socket has already registered a session. This is not allowed.
        // Return the already assigned session back, the cip spec is
        // not clear about this.
        *aSessionHandleResult = index + 1;
        return kEncapErrorInvalidOrUnsupportedCommand;
    }

    ses.m_is_registered = true;

    *aSessionHandleResult = index + 1;

    CIPSTER_TRACE_INFO( "%s[%d]: session_id:%d\n", __func__, aSocket, index + 1 );

    return kEncapErrorSuccess;
}


EncapSession* ServerSessionMgr::UpdateRegisteredTcpConnection( int aSocket )
{
    int index;

    for( index = 0; index < DIM(sessions); ++index )
    {
        if( sessions[index].m_socket == aSocket )
            break;
    }

    if( index == DIM(sessions) )
    {
        CIPSTER_TRACE_INFO( "%s[%d]: no socket match\n", __func__, aSocket );
        return NULL;
    }

    CIPSTER_TRACE_INFO( "%s[%d]: inactivity update\n", __func__, aSocket );
    sessions[index].m_last_activity_usecs = g_current_usecs;

    return &sessions[index];
}


EncapSession* ServerSessionMgr::CheckRegisteredSession(
        CipUdint aSessionHandle, int aSocket )
{
    // a session handle of zero is not legal, we also check for that.

    unsigned index = aSessionHandle - 1;    // goes very large posive at 0

    if( index < DIM( sessions )
     && sessions[index].m_socket == aSocket
     && sessions[index].m_is_registered )
    {
        return &sessions[index];
    }

    return NULL;
}


bool ServerSessionMgr::CloseBySessionHandle( CipUdint aSessionHandle )
{
    CIPSTER_ASSERT( aSessionHandle && aSessionHandle <= DIM(sessions) );

    unsigned index = aSessionHandle - 1;

    if( index < (unsigned) DIM(sessions) && sessions[index].m_socket != kSocketInvalid )
    {
        CIPSTER_TRACE_INFO( "%s[%d]: aSessionHandle:%d\n",
            __func__, sessions[index].m_socket, aSessionHandle );

        sessions[index].Close();
        return true;
    }
    else
    {
        CIPSTER_TRACE_INFO( "%s: BAD or inactive aSessionHandle:%d\n",
            __func__, aSessionHandle );

        return false;
    }
}


bool ServerSessionMgr::CloseBySocket( int aSocket )
{
    CIPSTER_TRACE_INFO( "%s[%d]\n", __func__, aSocket );

    for( int i = 0; i < DIM(sessions); ++i )
    {
        if( sessions[i].m_socket == aSocket )
        {
            sessions[i].Close();
            return true;
        }
    }

    return false;
}


EncapError ServerSessionMgr::UnregisterSession( CipUdint aSessionHandle, int aSocket )
{
    CIPSTER_TRACE_INFO( "%s[%d]: session_id:%d\n",
        __func__, aSocket, aSessionHandle );

    unsigned index = aSessionHandle - 1;

    if( index < DIM(sessions) )
    {
        if( sessions[index].m_socket == aSocket  )
        {
            sessions[index].Close();
            return kEncapErrorSuccess;
        }
    }

    // no such session registered
    return kEncapErrorInvalidSessionHandle;
}


void ServerSessionMgr::AgeInactivity()
{
    //CIPSTER_TRACE_INFO( "%s: \n", __func__ );

    // User can effectively defeat the inactivity timer by setting the attribute
    // to a large number of seconds.
    uint64_t timeout_usecs = CipTCPIPInterfaceInstance::inactivity_timeout_secs * 1000000;

    EncapSession* it  = sessions;
    EncapSession* end = it + DIM(sessions);

    for( ; it != end; ++it )
    {
        if( it->m_socket != kSocketInvalid )
        {
            // This is positive and valid for all values of g_current_usecs, even
            // if it has wrapped since setting it->m_last_activity_usecs.
            uint64_t age_usecs = g_current_usecs - it->m_last_activity_usecs;

            if( age_usecs >= timeout_usecs )
            {
                // Only a registered session can have Class3 or 4 connections.
                if( it->m_is_registered )
                {
                    // close any class3 connections associated with this socket.
                    CipUdint session_handle = (it - sessions) + 1;

                    CipConnMgrClass::CloseClass3Connections( session_handle );
                }

                CIPSTER_TRACE_INFO( "%s[%d]: >>>> TCP TIMEOUT\n", __func__, it->m_socket );
                it->Close();
            }
        }
    }
}


void ServerSessionMgr::Shutdown()
{
    EncapSession* it  = sessions;
    EncapSession* end = it + DIM(sessions);

    for( ; it != end; ++it )
    {
        if( it->m_socket != kSocketInvalid )
        {
            it->Close();
        }
    }
}

//-----<Encapsulation>----------------------------------------------------------



static const char* ShowEncapCmd( int aCmd )
{
    static char unknown[16];

    switch( EncapCmd( aCmd ) )
    {
    case kEncapCmdNoOperation:          return "NoOperation";
    case kEncapCmdListServices:         return "ListServices";
    case kEncapCmdListIdentity:         return "ListIdentity";
    case kEncapCmdListInterfaces:       return "ListInterfaces";
    case kEncapCmdRegisterSession:      return "RegisterSession";
    case kEncapCmdUnregisterSession:    return "UnregisterSession";
    case kEncapCmdSendRRData:           return "SendRRData";
    case kEncapCmdSendUnitData:         return "SendUnitData";
    default:
            snprintf( unknown, sizeof unknown, "?=0x%x", aCmd );
            return unknown;
    }
}


void Encapsulation::Init()
{
    int stack_var;

    srand( (unsigned) (uintptr_t) &stack_var );

    ServerSessionMgr::Init();
}


void Encapsulation::ShutDown()
{
    ServerSessionMgr::Shutdown();
}


int Encapsulation::EnsuredTcpRecv( int aSocket, EipByte* aDest, int aByteCount )
{
    int i;
    int numRead;

    for( i=0;  i < aByteCount;  i += numRead )
    {
        numRead = recv( aSocket, (char*) aDest+i, aByteCount - i, 0 );
        if( numRead == 0 )
            break;

        else if( numRead == -1 )
        {
            i = -1;
            break;
        }
    }

    return i;
}


/**
 * Function disposeOfLargePacket
 *
 * @return int - >  0 : number of bytes read.
 *             - == 0 : socket close on other end.
 *             - <  0 : error of some kind.
 */
static int disposeOfLargePacket( int aSocket, unsigned aCount )
{
    EipByte chunk[256];
    int     total = 0;

    CIPSTER_TRACE_INFO( "%s[%d]: count:%zd\n", __func__, aSocket, aCount );

    while( aCount )
    {
        // toss in chunks.
        int readz = std::min( aCount, unsigned( sizeof chunk ) );

        int num_read = Encapsulation::EnsuredTcpRecv( aSocket, chunk, readz );

        if( num_read != readz )
        {
            return num_read;
        }

#if defined(DEBUG) && 0
        byte_dump( "bigTCP", chunk, num_read );
#endif
        total  += num_read;
        aCount -= num_read;
    }

    //CIPSTER_TRACE_INFO( "~%s\n", __func__ );

    return total;
}


int Encapsulation::ReceiveTcpMsg( int aSocket, BufWriter aMsg )
{
    EipByte* start = aMsg.data();

    CIPSTER_TRACE_INFO( "%s[%d]:\n", __func__, aSocket );

    if( aMsg.capacity() < ENCAPSULATION_HEADER_LENGTH )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: aMsg size is too small\n", __func__, aSocket );
        return -1;
    }

    int num_read = EnsuredTcpRecv( aSocket, aMsg.data(), ENCAPSULATION_HEADER_LENGTH );

    if( num_read == 0 )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: other end of socket closed by client\n",
                __func__, aSocket );
        return -1;
    }

    if( num_read < ENCAPSULATION_HEADER_LENGTH )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: recv() error: %s\n",
                __func__, aSocket, strerror(errno) );
        return -1;
    }

    unsigned remaining = start[2] | (start[3] << 8);

    // advance BufWriter's pointer
    aMsg += ENCAPSULATION_HEADER_LENGTH;

    if( remaining > aMsg.capacity() )
    {
        if( remaining > 65511 )
        {
            CIPSTER_TRACE_ERR(
                "%s[%d]: illegal encapsulation data size:%d\n"
                " possibly out of sync, closing TCP connection\n",
                __func__, aSocket, remaining );
            return kEipStatusError;
        }

#if defined(DEBUG)
        byte_dump( "rHDR", start, ENCAPSULATION_HEADER_LENGTH );
#endif

        CIPSTER_TRACE_ERR(
            "%s[%d]: packet len=%u is too big for #defined CIPSTER_ETHERNET_BUFFER_SIZE,\n"
            "ignoring entire packet with Encap.command='%s'\n",
            __func__, aSocket, remaining + ENCAPSULATION_HEADER_LENGTH,
            ShowEncapCmd( start[0] | (start[1] << 8) )
            );

        if( disposeOfLargePacket( aSocket, remaining ) != remaining )
            return kEipStatusError;
        else
            return remaining + ENCAPSULATION_HEADER_LENGTH;
    }

    num_read = EnsuredTcpRecv( aSocket, aMsg.data(), remaining );

    if( num_read < 0 )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: error on recv: %s\n",
                __func__, aSocket, strerror(errno) );
        return kEipStatusError;
    }

    if( num_read < remaining )      // connection closed by client
    {
        CIPSTER_TRACE_ERR( "%s[%d]: connection closed by client: %s\n",
                __func__, aSocket, strerror(errno) );
        return kEipStatusError;
    }

    num_read += ENCAPSULATION_HEADER_LENGTH;

#if defined(DEBUG)
    byte_dump( "rTCP", start, num_read );
#endif

    (void) start;

    CIPSTER_TRACE_INFO( "%s[%d]: received %d TCP bytes, command:'%s'\n",
            __func__, aSocket, num_read,
            ShowEncapCmd( start[0] | (start[1] << 8) )
            );

    return num_read;
}


const int kListIdentityDefaultDelayTime = 2000;
const int kListIdentityMinimumDelayTime = 500;

static unsigned determineDelayTimeMSecs( unsigned aMaxMSecs )
{
    /*  The receiver’s delay shall be a random value, in milliseconds,
        between 0 and the MaxResponseDelay specified in the ListIdentity
        request. If the sender specifies a MaxResponseDelay value of 0
        ms, a default value of 2000 ms shall be used by the receiver. If
        the sender specifies a MaxResponseDelay value of 1-500 ms, a
        value of 500 ms shall be used by the receiver. A new random
        value shall be chosen for each request.
    */

    if( aMaxMSecs == 0 )
    {
        aMaxMSecs = kListIdentityDefaultDelayTime;
    }
    else if( aMaxMSecs < kListIdentityMinimumDelayTime )
    {
        // if seed time is between 1 and 500ms inclusive, set it to 500ms
        aMaxMSecs = kListIdentityMinimumDelayTime;
    }

    // Set delay time randomly between 0 and aMaxMSecs, use 64 bit unsigned
    // integer math to scale the aMaxMSecs between 0 and aMaxMSecs.  Without
    // 64 bit math the multiply in this calculation overflows using 32 bit ints.
    // Alternatively could use float or double in the product before coming
    // back to unsigned result.
    unsigned ret = ( uint64_t(aMaxMSecs) * rand() ) / RAND_MAX;

    return ret;
}


int Encapsulation::registerSession( int socket, BufReader aCommand, BufWriter aReply,
        EncapError* aEncapError, CipUdint* aSessionHandleResult )
{
    *aEncapError = kEncapErrorSuccess;

    BufWriter out = aReply;

    int version = aCommand.get16();
    int options = aCommand.get16();

    // Check if requested protocol version is supported and the
    // register session option parameter is zero.
    if( version && version <= kSupportedProtocolVersion && !options )
    {
        *aEncapError = ServerSessionMgr::RegisterSession( socket, aSessionHandleResult );
    }
    else    // protocol not supported
    {
        *aEncapError = kEncapErrorUnsupportedProtocol;
    }

    if( version > kSupportedProtocolVersion )
        version = kSupportedProtocolVersion;

    out.put16( version );
    out.put16( options );

    return out.data() - aReply.data();
}


int Encapsulation::serializeListIdentityResponse( BufWriter aReply )
{
    BufWriter out = aReply;

    out.put16( 1 );       // Item count: one item
    out.put16( kCpfIdListIdentityResponse );

    // at this byte offset the real length will be inserted below
    BufWriter id_length = out;

    out += 2;   // skip over deferred id_length slot.

    out.put16( kSupportedProtocolVersion )

    .put16BE( AF_INET )
    .put16BE( kEIP_Reserved_Port )
    .put32BE( ntohl( CipTCPIPInterfaceClass::InterfaceConf( 1 ).ip_address ) )

    .fill( 8 )

    .put16( vendor_id_ ).put16( device_type_ ).put16( product_code_ )

    .put8( revision_.major_revision ).put8( revision_.minor_revision )

    .put16( status_ )
    .put32( serial_number_ )

    .put_SHORT_STRING( product_name_, false )

    .put8( 0xff );      // optional STATE, not supported indicated by 0xff.

    // the -2 is for not counting the length field
    id_length.put16( out.data() - id_length.data() - 2 );

    return out.data() - aReply.data();
}


int Encapsulation::handleReceivedListIdentityCommandImmediate( BufWriter aReply )
{
    return serializeListIdentityResponse( aReply );
}


int Encapsulation::handleReceivedListIdentityCommandDelayed(
        int aSocket, const SockAddr& aFromAddress,
        unsigned aMSecDelay, BufReader aCommand )
{
    DelayedMsg* delayed = NULL;

    for( unsigned i = 0; i < DIM( DelayedMsg::messages );  ++i )
    {
        if( kSocketInvalid == DelayedMsg::messages[i].socket )
        {
            delayed = &DelayedMsg::messages[i];
            break;
        }
    }

    if( delayed )
    {
        delayed->socket   = aSocket;
        delayed->receiver = aFromAddress;

        delayed->time_out_usecs = aMSecDelay * 1000;

        BufWriter out( delayed->message, sizeof delayed->message );

        out.append( aCommand.data(), ENCAPSULATION_HEADER_LENGTH );

        delayed->message_size = serializeListIdentityResponse( out );

        BufWriter len( delayed->message + 2, 2 );

        len.put16( delayed->message_size );

        delayed->message_size += ENCAPSULATION_HEADER_LENGTH;
    }

    return 0;
}


int Encapsulation::handleReceivedListInterfacesCommand( BufWriter aReply )
{
    // Vol2 2-4.3.3 At present no public items are defined for the ListInterfaces reply.
    aReply.put16( 0 );

    return 2;
}


/**
 * Function handleReceivedListServicesCommand
 * generates a reply with "Communications" + compatibility Flags.
 * @param aReply where to put the reply
 * @return int - the number of bytes put into aReply or -1 if buffer overrun.
 */
int Encapsulation::handleReceivedListServicesCommand( BufWriter aReply )
{
    BufWriter out = aReply;

    static const EipByte name_of_service[16] = "Communications";

    try
    {
        out.put16( 1 )
        .put16( kCpfIdListServiceResponse )
        .put16( 20 )        // length of following command specific data is fixed
        .put16( 1 )         // protocol version
        .put16( kCapabilityFlagsCipTcp | kCapabilityFlagsCipUdpClass0or1 ) // capability_flags
        .append( name_of_service, 16 );

        return out.data() - aReply.data();
    }
    catch( const std::exception& e )
    {
        CIPSTER_TRACE_ERR( "%s: buffer overrun\n", __func__ );
        return -1;
    }
}


int Encapsulation::HandleReceivedExplicitTcpData( int aSocket,
        BufReader aCommand, BufWriter aReply )
{
    CIPSTER_ASSERT( aReply.capacity() >= ENCAPSULATION_HEADER_LENGTH ); // caller bug

    int result = -1;

    if( aCommand.size() < ENCAPSULATION_HEADER_LENGTH )
    {
        CIPSTER_TRACE_INFO( "%s[%d]: aCommand too small\n", __func__, aSocket );
        return -1;
    }

    // kick the TCP inactivity watchdog timer for this socket
    ServerSessionMgr::UpdateRegisteredTcpConnection( aSocket );

    Encapsulation encap;

    int headerz = encap.DeserializeEncap( aCommand );

    if( headerz != encap.HeaderLength() )
    {
        CIPSTER_TRACE_INFO( "%s[%d]: Deserialized header size invalid\n",
            __func__, aSocket );
        return -1;
    }

    if( encap.Command() == kEncapCmdNoOperation )
    {
        /*  Either an originator or a target may send a NOP command. No reply
            shall be generated by this command. The data portion of the command
            shall be from 0 to 65511 bytes long. The receiver shall ignore any
            data that is contained in the message. A NOP command does not
            require that a session be established.
        */

        CIPSTER_TRACE_INFO( "%s[%d]:'NOP' not gonna reply\n", __func__, aSocket );

        return 0;       // This is not an error, but we should not reply.
    }

    if( encap.Status() )  // all commands have 0 in status, else ignore with no reply
    {
        CIPSTER_TRACE_INFO( "%s[%d]: header status != 0\n", __func__, aSocket );
        return 0;       // Zero keeps the TCP connection open, but omits a reply.
    }

    if( encap.Options() )  // all commands have 0 in options, else ignore with no reply
    {
        CIPSTER_TRACE_INFO( "%s[%d]: header status != 0\n", __func__, aSocket );
        return 0;       // Zero keeps the TCP connection open, but omits a reply.
    }

    // Establish default Encapsulation length field, maybe adjusted at bottom here.
    encap.SetPayloadLength( 0 );

    // Adjust for encapsulation header which is both consumed in the command
    // and reserved in the reply, i.e. skip over these bytes for both.
    BufReader   command = aCommand + headerz;
    BufWriter   reply   = aReply + headerz;

    CIPSTER_TRACE_INFO( "%s[%d]:'%s'\n",
        __func__, aSocket, ShowEncapCmd( encap.Command() ) );

    // most of these functions need a reply to be sent
    switch( encap.Command() )
    {
    case kEncapCmdListServices:
        if( !encap.Options() )
            result = handleReceivedListServicesCommand( reply );
        break;

    case kEncapCmdListIdentity:
        if( !encap.Options() )
            result = handleReceivedListIdentityCommandImmediate( reply );
        break;

    case kEncapCmdListInterfaces:
        if( !encap.Options() )
            result = handleReceivedListInterfacesCommand( reply );
        break;

    case kEncapCmdRegisterSession:
        if( !encap.Options() )
        {
            EncapError status;
            CipUdint   sh = 0;

            result = registerSession( aSocket, command, reply, &status, &sh );
            encap.SetSessionHandle( sh );
            encap.SetStatus( status );
        }
        break;

    case kEncapCmdUnregisterSession:
        ServerSessionMgr::UnregisterSession( encap.SessionHandle(), aSocket );

        CIPSTER_TRACE_INFO(
            "%s[%d]: no reply required for encap UnregisterSession\n",
            __func__, aSocket );

        // no reply, but since ServerSessionMgr::UnregisterSession() has already
        // closed the socket, return 0 so caller does not also.
        result = 0;
        break;

    case kEncapCmdSendRRData:
        if( !encap.Options() && command.size() )
        {
            EncapSession* ses = ServerSessionMgr::CheckRegisteredSession(
                                    encap.SessionHandle(), aSocket );

            if( ses )
            {
                Cpf cpf( &ses->m_peeraddr ) ;

                result = cpf.NotifyCommonPacketFormat(
                            command,    // past encap header (headerz)
                            reply       // past encap header (headerz)
                            );

                if( result < 0 )
                {
                    encap.SetStatus( -result );

                    // reply with encap.Status() != 0, and only send encap header.
                    result = 0;
                }
            }
            else    // received a packet with non registered session handle
            {
                CIPSTER_TRACE_ERR( "%s[%d]: InvalidSessionHandle:%d\n",
                    __func__, aSocket, encap.SessionHandle() );

                encap.SetStatus( kEncapErrorInvalidSessionHandle );
                result = 0;
            }
        }
        break;

    case kEncapCmdSendUnitData:
        if( !encap.Options() && command.size() )
        {
            EncapSession* ses = ServerSessionMgr::CheckRegisteredSession(
                                    encap.SessionHandle(), aSocket );

            if( ses )
            {
                Cpf cpf( &ses->m_peeraddr ) ;

                result = cpf.NotifyConnectedCommonPacketFormat(
                            command,    // past encap header
                            reply       // past encap header
                            );
            }
            else    // received a packet with non registered session handle
            {
                CIPSTER_TRACE_ERR( "%s[%d]: InvalidSessionHandle:%d\n",
                    __func__, aSocket, encap.SessionHandle() );

                encap.SetStatus( kEncapErrorInvalidSessionHandle );
                result = 0;
            }
        }
        break;

    default:
        CIPSTER_TRACE_INFO( "%s[%d]: unexpected command:0x%x\n",
            __func__, aSocket, encap.Command() );

        // Vol2 2-3.2
        encap.SetStatus( kEncapErrorInvalidOrUnsupportedCommand );
        result = 0;
        break;
    }

    if( result >= 0 )
    {
        encap.SetPayloadLength( result );
        result += encap.Serialize( aReply );
    }

    return result;
}


int Encapsulation::HandleReceivedExplicitUdpData( int aSocket,
        const SockAddr& aFromAddress,
        BufReader aCommand, BufWriter aReply, bool isUnicast )
{
    CIPSTER_ASSERT( aReply.capacity() >= ENCAPSULATION_HEADER_LENGTH ); // caller bug

    if( aCommand.size() < ENCAPSULATION_HEADER_LENGTH )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: aCommand.size too small\n", __func__, aSocket );
        return -1;
    }

    Encapsulation encap;

    int result = encap.DeserializeEncap( aCommand );

    if( result != ENCAPSULATION_HEADER_LENGTH )
    {
        CIPSTER_TRACE_ERR( "%s[%d]: unable to DeserializeEncap, result=%d\n",
            __func__, aSocket, result );
        return -1;
    }

    if( encap.Command() == kEncapCmdNoOperation )
    {
        CIPSTER_TRACE_INFO( "%s[%d]: NOP ignored\n",  __func__, aSocket );
        return -1;
    }

    CIPSTER_TRACE_INFO( "%s[%d]: encap.Command():'%s' payload_size:%d\n",
        __func__,
        aSocket,
        ShowEncapCmd( encap.Command() ),
        (int) aCommand.size()
        );

    if( encap.Status() )  // all commands have 0 in status, else ignore
    {
        CIPSTER_TRACE_ERR( "%s[%d]: encap.Status() != 0\n", __func__, aSocket );
        return -1;
    }

    encap.SetPayloadLength( 0 );  // aCommand.size() has our length, establish default for reply

    // Adjust locally for encapsulation header which is both consumed in the
    // the command and reserved in the reply.
    //BufReader   command = aCommand + ENCAPSULATION_HEADER_LENGTH;
    BufWriter   reply = aReply + ENCAPSULATION_HEADER_LENGTH;

    switch( encap.Command() )
    {
    case kEncapCmdListServices:
        if( !encap.Options() )
            result = handleReceivedListServicesCommand( reply );
        break;

    case kEncapCmdListIdentity:
        if( isUnicast )
        {
            result = handleReceivedListIdentityCommandImmediate( reply );
        }
        else
        {
            unsigned maxMSecs = encap.TimeoutMS();
            unsigned timeoutMSecs = determineDelayTimeMSecs( maxMSecs );

            result = handleReceivedListIdentityCommandDelayed( aSocket, aFromAddress,
                        timeoutMSecs, aCommand );
        }
        break;

    case kEncapCmdListInterfaces:
        if( !encap.Options() )
            result = handleReceivedListInterfacesCommand( reply );
        break;

    // The following commands are not to be sent via UDP
    case kEncapCmdNoOperation:
    case kEncapCmdRegisterSession:
    case kEncapCmdUnregisterSession:
    case kEncapCmdSendRRData:
    case kEncapCmdSendUnitData:
    default:
        result = -1;
        encap.SetStatus( kEncapErrorInvalidOrUnsupportedCommand );
        break;
    }

    // if result is greater than 0, then data has to be sent.  If it is precisely
    // zero, then we arrived here from a handleReceivedListIdentityCommandDelayed()
    // call and should not send now but will send later.
    if( result > 0 )
    {
        encap.SetPayloadLength( result );
        encap.Serialize( aReply );
        result += ENCAPSULATION_HEADER_LENGTH;
    }

    CIPSTER_TRACE_INFO( "%s[%d]: ret:%d\n", __func__, aSocket, result );

    return result;
}


int Encapsulation::DeserializeEncap( BufReader aCommand )
{
    BufReader in = aCommand;

    command = (EncapCmd) in.get16();
    length  = in.get16();

    session_handle = in.get32();

    status = in.get32();

    for( int i=0; i<8;  ++i )
        sender_context[i] = in.get8();

    options = in.get32();

    if( IsBigHdr() )
    {
        interface_handle = in.get32();
        timeout = in.get16();
    }
    else
    {
        interface_handle = 0;
        timeout = 0;
    }

    int byte_count = in.data() - aCommand.data();

    return byte_count;
}


int Encapsulation::SerializedCount( int aCtl ) const
{
    return HeaderLength();
}


int Encapsulation::Serialize( BufWriter aDst, int aCtl ) const
{
    BufWriter out = aDst;
    CipUint   len = length; // plan to adjust length based on payload

    if( payload )
    {
        len = payload->SerializedCount( aCtl );

        if( IsBigHdr() )
            len += 6;
    }

    out.put16( command )
    .put16( len )
    .put32( session_handle )
    .put32( status )
    .append( (EipByte*) sender_context, 8 )
    .put32( options );

    if( IsBigHdr() )
    {
        out.put32( interface_handle );
        out.put16( timeout );
    }

    if( payload )
    {
        out += payload->Serialize( out, aCtl );
    }

    return out.data() - aDst.data();
}


void ManageEncapsulationMessages()
{
    for( int i = 0; i < DIM( DelayedMsg::messages );  ++i )
    {
        if( kSocketInvalid != DelayedMsg::messages[i].socket )
        {
            DelayedMsg::messages[i].time_out_usecs -= kOpenerTimerTickInMicroSeconds;

            if( DelayedMsg::messages[i].time_out_usecs < 0 )
            {
                // If delay is reached or passed, send the UDP message
                SendUdpData( DelayedMsg::messages[i].receiver,
                        DelayedMsg::messages[i].socket,
                        DelayedMsg::messages[i].Payload() );

                DelayedMsg::messages[i].socket = -1;
            }
        }
    }
}