#include "client.h"

#include "globals.h"
#include "log.h"
#include "my_malloc.h"
#include "structured_stream.h"
#include "time_utils.h"

#include <assert.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int menoetius_client_init( struct menoetius_client* client, const char* server, int port )
{
	client->server = server;
	client->port = port;
	client->read_buf_size = 1024;
	client->write_buf_size = 1024;

	client->fd = -1;

	return 0;
}

void menoetius_client_shutdown( struct menoetius_client* client )
{
	if( client->fd >= 0 ) {
		close( client->fd );
		client->fd = -1;
	}
	if( client->ss ) {
		structured_stream_free( client->ss );
		client->ss = NULL;
	}
}

int menoetius_client_ensure_connected( struct menoetius_client* client )
{
	int res = 0;

	if( client->fd >= 0 ) {
		assert( client->ss );
		LOG_DEBUG( "reusing existing client connection" );
		return 0;
	}

	client->fd = socket( AF_INET, SOCK_STREAM, 0 );
	if( client->fd < 0 ) {
		LOG_ERROR( "error opening socket" );
		return 1;
	}

	struct hostent* server = gethostbyname( client->server );
	if( server == NULL ) {
		LOG_ERROR( "hostname=s error looking up hostname", client->server );
		close( client->fd );
		client->fd = -1;
		return 1;
	}

	struct sockaddr_in serveraddr;
	bzero( (char*)&serveraddr, sizeof( serveraddr ) );
	serveraddr.sin_family = AF_INET;
	bcopy( (char*)server->h_addr, (char*)&serveraddr.sin_addr.s_addr, server->h_length );
	serveraddr.sin_port = htons( client->port );

	/* connect: create a connection with the server */
	if( connect( client->fd, (const struct sockaddr*)&serveraddr, sizeof( serveraddr ) ) < 0 ) {
		LOG_ERROR( "hostname=s failed to connect", client->server );
		close( client->fd );
		client->fd = -1;
		return 1;
	}

	res = structured_stream_new(
		client->fd, client->read_buf_size, client->write_buf_size, &( client->ss ) );
	if( res ) {
		return res;
	}

	uint64_t magic_header = 1547675033;
	if( ( res = structured_stream_write_uint64( client->ss, magic_header ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}
	LOG_DEBUG( "established new client connection" );
	return 0;
}

int menoetius_client_send( struct menoetius_client* client,
						   const char* key,
						   size_t key_len,
						   size_t num_pts,
						   int64_t* t,
						   double* y )
{
	int i;
	int res;

	if( ( res = menoetius_client_ensure_connected( client ) ) ) {
		LOG_ERROR( "res=s failed to connect", err_str( res ) );
		return res;
	}

	if( ( res = structured_stream_write_uint8( client->ss, MENOETIUS_RPC_PUT_DATA ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( ( res = structured_stream_write_uint16_prefixed_bytes( client->ss, key, key_len ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( ( res = structured_stream_write_uint32( client->ss, num_pts ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	for( i = 0; i < num_pts; i++ ) {
		if( ( res = structured_stream_write_uint64( client->ss, t[i] ) ) ) {
			LOG_ERROR( "res=s write failed", err_str( res ) );
			return res;
		}

		if( ( res = structured_stream_write_double( client->ss, y[i] ) ) ) {
			LOG_ERROR( "res=s write failed", err_str( res ) );
			return res;
		}
	}

	// end of batch marker (empty string "")
	if( ( res = structured_stream_write_uint16_prefixed_bytes( client->ss, NULL, 0 ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		return res;
	}

	if( ( res = structured_stream_flush( client->ss ) ) ) {
		LOG_ERROR( "res=s flush failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	// wait for response from server
	uint8_t server_response;
	if( ( res = structured_stream_read_uint8( client->ss, &server_response ) ) ) {
		LOG_ERROR( "res=s failed to read", err_str( res ) );
		return res;
	}

	// ignore out of date response for now
	server_response &= ~MENOETIUS_CLUSTER_CONFIG_OUT_OF_DATE;

	LOG_INFO( "num_pts=d resp=d sent points", num_pts, server_response );
	return server_response;
}

int menoetius_client_get( struct menoetius_client* client,
						  const char* key,
						  size_t key_len,
						  size_t max_num_pts,
						  size_t* num_pts,
						  int64_t* t,
						  double* y )
{
	int res;

	if( ( res = menoetius_client_ensure_connected( client ) ) ) {
		LOG_ERROR( "res=s failed to connect", err_str( res ) );
		return res;
	}

	if( ( res = structured_stream_write_uint8( client->ss, MENOETIUS_RPC_GET_DATA ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	// write num of keys
	if( ( res = structured_stream_write_uint16( client->ss, 1 ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	// write keys
	if( ( res = structured_stream_write_uint16_prefixed_bytes( client->ss, key, key_len ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	// flush
	if( ( res = structured_stream_flush( client->ss ) ) ) {
		LOG_ERROR( "res=s flush failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	// read num points
	uint32_t num_pts_server;
	if( ( res = structured_stream_read_uint32( client->ss, &num_pts_server ) ) ) {
		LOG_ERROR( "res=s read failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( num_pts_server > max_num_pts ) {
		LOG_ERROR( "num=d too many points received from server; increase client memory", num_pts );
		menoetius_client_shutdown( client );
		return 1;
	}

	for( uint32_t j = 0; j < num_pts_server; j++ ) {
		if( ( res = structured_stream_read_int64( client->ss, &t[j] ) ) ) {
			LOG_ERROR( "res=s read failed", err_str( res ) );
			menoetius_client_shutdown( client );
			return res;
		}
		if( ( res = structured_stream_read_double( client->ss, &y[j] ) ) ) {
			LOG_ERROR( "res=s read failed", err_str( res ) );
			menoetius_client_shutdown( client );
			return res;
		}
	}

	// read response code
	uint8_t server_response;
	if( ( res = structured_stream_read_uint8( client->ss, &server_response ) ) ) {
		LOG_ERROR( "res=s read failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	// ignore out of date response for now
	server_response &= ~MENOETIUS_CLUSTER_CONFIG_OUT_OF_DATE;

	*num_pts = num_pts_server;
	return server_response;
}

int menoetius_client_get_status( struct menoetius_client* client, int* status )
{
	int res;

	if( ( res = menoetius_client_ensure_connected( client ) ) ) {
		LOG_ERROR( "res=s failed to connect", err_str( res ) );
		return res;
	}

	if( ( res = structured_stream_write_uint8( client->ss, MENOETIUS_RPC_GET_STATUS ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( ( res = structured_stream_flush( client->ss ) ) ) {
		LOG_ERROR( "res=s flush failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	uint8_t tmp;
	if( ( res = structured_stream_read_uint8( client->ss, &tmp ) ) ) {
		LOG_ERROR( "res=s read failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	*status = tmp;
	return 0;
}

int menoetius_client_get_cluster_config( struct menoetius_client* client )
{
	int res;

	if( ( res = menoetius_client_ensure_connected( client ) ) ) {
		LOG_ERROR( "res=s failed to connect", err_str( res ) );
		return res;
	}

	if( ( res = structured_stream_write_uint8( client->ss, MENOETIUS_RPC_GET_CLUSTER_CONFIG ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( ( res = structured_stream_flush( client->ss ) ) ) {
		LOG_ERROR( "res=s flush failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	const char* hash;
	if( ( res = structured_stream_read_bytes_inplace( client->ss, HASH_LENGTH, &hash ) ) ) {
		LOG_ERROR( "res=s read failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	size_t n;
	const char* cluster_config;
	if( ( res = structured_stream_read_uint16_prefixed_bytes_inplace(
			  client->ss, &cluster_config, &n ) ) ) {
		LOG_ERROR( "res=s read failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}
	LOG_INFO( "n=d got the config", n );

	return 0;
}

int menoetius_client_test_hook( struct menoetius_client* client, uint64_t flags )
{
	int res;

	if( ( res = menoetius_client_ensure_connected( client ) ) ) {
		LOG_ERROR( "res=s failed to connect", err_str( res ) );
		return res;
	}

	if( ( res = structured_stream_write_uint8( client->ss, MENOETIUS_RPC_TEST_HOOK ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( ( res = structured_stream_write_uint64( client->ss, flags ) ) ) {
		LOG_ERROR( "res=s write failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	if( ( res = structured_stream_flush( client->ss ) ) ) {
		LOG_ERROR( "res=s flush failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	uint8_t server_status;
	if( ( res = structured_stream_read_uint8( client->ss, &server_status ) ) ) {
		LOG_ERROR( "res=s read failed", err_str( res ) );
		menoetius_client_shutdown( client );
		return res;
	}

	return server_status;
}
