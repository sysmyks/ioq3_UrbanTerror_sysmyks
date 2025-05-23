/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// sv_client.c -- server code for dealing with clients

#include "server.h"
static void SV_CloseDownload( client_t *cl );

/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

If we are authorizing, a challenge request will cause a packet
to be sent to the authorize server.

When an authorizeip is returned, a challenge response will be
sent to that ip.
=================
*/
void SV_GetChallenge(netadr_t from) {

    int          i;
    int          oldest;
    int          oldestTime;
    challenge_t  *challenge;

    oldest = 0;
    oldestTime = 0x7fffffff;

    // see if we already have a challenge for this ip
    challenge = &svs.challenges[0];
    for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {

        if (!challenge->connected && NET_CompareAdr(from, challenge->adr)) {
            break;
        }

        if (challenge->time < oldestTime) {
            oldestTime = challenge->time;
            oldest = i;
        }

    }

    if (i == MAX_CHALLENGES) {

        // this is the first time this client has asked for a challenge
        challenge = &svs.challenges[oldest];

        challenge->challenge = ((rand() << 16) ^ rand()) ^ svs.time;
        challenge->adr = from;
        challenge->pingTime = -1;
        challenge->firstTime = svs.time;
        challenge->time = svs.time;
        challenge->connected = qfalse;
        i = oldest;

    }

    if (challenge->pingTime == -1) {
        challenge->pingTime = svs.time;
    }

    NET_OutOfBandPrint(NS_SERVER, from, "challengeResponse %i", challenge->challenge);
    return;

}

/////////////////////////////////////////////////////////////////////
// SV_ApproveGuid
// --------------
// A cl_guid string must have length 32 and consist of characters '0'
// through '9' and 'A' through 'F'.
/////////////////////////////////////////////////////////////////////
qboolean SV_ApproveGuid(const char *guid) {

    int    i;
    int    length;
    char   c;
    
    if (mod_checkClientGuid->integer > 0) {

        length = strlen(guid); 
        if (length != 32) { 
            return qfalse; 
        }

        for (i = 31; i >= 0;) {
            c = guid[i--];
            if (!(('0' <= c && c <= '9') || ('A' <= c && c <= 'F'))) {
                return qfalse;
            }
        }
    }
    return qtrue;
}

/*
====================
SV_AuthorizeIpPacket

A packet has been returned from the authorize server.
If we have a challenge adr for that ip, send the
challengeResponse to it
====================
*/
void SV_AuthorizeIpPacket( netadr_t from ) {
	int		challenge;
	int		i;
	char	*s;
	char	*r;

	if ( !NET_CompareBaseAdr( from, svs.authorizeAddress ) ) {
		Com_Printf( "SV_AuthorizeIpPacket: not from authorize server\n" );
		return;
	}

	challenge = atoi( Cmd_Argv( 1 ) );

	for (i = 0 ; i < MAX_CHALLENGES ; i++) {
		if ( svs.challenges[i].challenge == challenge ) {
			break;
		}
	}
	if ( i == MAX_CHALLENGES ) {
		Com_Printf( "SV_AuthorizeIpPacket: challenge not found\n" );
		return;
	}

	// send a packet back to the original client
	if (svs.challenges[i].pingTime == -1) {
		svs.challenges[i].pingTime = svs.time; 
	}

	s = Cmd_Argv( 2 );
	r = Cmd_Argv( 3 );			// reason

	if ( !Q_stricmp( s, "demo" ) ) {
		// they are a demo client trying to connect to a real server
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nServer is not a demo server\n" );
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
		return;
	}
	if ( !Q_stricmp( s, "accept" ) ) {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, 
			"challengeResponse %i", svs.challenges[i].challenge );
		return;
	}
	if ( !Q_stricmp( s, "unknown" ) ) {
		if (!r) {
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nAwaiting CD key authorization\n" );
		} else {
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\n%s\n", r);
		}
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
		return;
	}

	// authorization failed
	if (!r) {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nSomeone is using this CD Key\n" );
	} else {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\n%s\n", r );
	}

	// clear the challenge record so it won't timeout and let them through
	Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/

#define PB_MESSAGE "PunkBuster Anti-Cheat software must be installed " \
                   "and Enabled in order to join this server. An updated game patch can be downloaded from " \
                   "www.idsoftware.com"

void SV_DirectConnect(netadr_t from) {

    char            userinfo[MAX_INFO_STRING];
    int             i, j;
    client_t        temp;
    client_t       *cl, *newcl;
    sharedEntity_t *ent;
    char           *password;
    int             clientNum;
    int             version;
    int             qport;
    int             challenge;
    int             startIndex;
    intptr_t        denied;
    int             count;
    int             numIpClients = 0;

    Com_DPrintf("SV_DirectConnect()\n");

    Q_strncpyz(userinfo, Cmd_Argv(1), sizeof(userinfo));

    version = atoi(Info_ValueForKey(userinfo, "protocol"));
    if (version != PROTOCOL_VERSION) {
        NET_OutOfBandPrint(NS_SERVER, from, "print\nServer uses protocol version %i.\n", PROTOCOL_VERSION);
        Com_DPrintf("    rejected connect from version %i\n", version);
        return;
    }

    challenge = atoi(Info_ValueForKey(userinfo, "challenge"));
    qport = atoi(Info_ValueForKey(userinfo, "qport"));

    // quick reject
    for (i = 0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

        if (cl->state == CS_FREE) {
            continue;
        }

        if (NET_CompareBaseAdr(from, cl->netchan.remoteAddress) && (cl->netchan.qport == qport || from.port == cl->netchan.remoteAddress.port)) {

            if ((svs.time - cl->lastConnectTime) < (sv_reconnectlimit->integer * 1000)) {
                Com_DPrintf("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
                return;
            }

            break;
        }
    }

    // see if the challenge is valid (LAN clients don't need to challenge)
    if (!NET_IsLocalAddress (from)) {

        int ping;

        for (i = 0 ; i < MAX_CHALLENGES; i++) {
            if (NET_CompareAdr(from, svs.challenges[i].adr)) {
                if (challenge == svs.challenges[i].challenge) {
                    break;  // good
                }
            }
        }

        if (i == MAX_CHALLENGES) {
            NET_OutOfBandPrint(NS_SERVER, from, "print\nNo or bad challenge for address.\n");
            return;
        }

        // force the IP key/value pair so the game can filter based on ip
        Info_SetValueForKey(userinfo, "ip", NET_AdrToString(from));

        // Note that it is totally possible to flood the console and qconsole.log by being rejected
        // (high ping, ban, server full, or other) and repeatedly sending a connect packet against the same
        // challenge.  Prevent this situation by only logging the first time we hit SV_DirectConnect()
        // for this challenge.
        if (!svs.challenges[i].connected) {
            ping = svs.time - svs.challenges[i].pingTime;
            svs.challenges[i].challengePing = ping;
            Com_Printf("Client %i connecting with %i challenge ping\n", i, ping);
        }
        else {
            ping = svs.challenges[i].challengePing;
            Com_DPrintf("Client %i connecting again with %i challenge ping\n", i, ping);
        }

        svs.challenges[i].connected = qtrue;

        // never reject a LAN client based on ping
        if (!Sys_IsLANAddress(from)) {

            for (j=0,cl=svs.clients ; j < sv_maxclients->integer ; j++,cl++) {
                if ( cl->state == CS_FREE ) {
                    continue;
                }   
                if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
                    && !( cl->netchan.qport == qport 
                    || from.port == cl->netchan.remoteAddress.port ) ) {
                    numIpClients++; 
                }   
            }

            if (sv_clientsPerIp->integer && numIpClients >= sv_clientsPerIp->integer) {
                NET_OutOfBandPrint(NS_SERVER, from, "print\nToo many connections from the same IP\n");
                Com_DPrintf ("Client %i rejected due to too many connections from the same IP\n", i);
                return;
            }

            if (sv_minPing->value && ping < sv_minPing->value) {
                NET_OutOfBandPrint(NS_SERVER, from, "print\nServer is for high pings only\n");
                Com_DPrintf ("Client %i rejected on a too low ping\n", i);
                return;
            }

            if (sv_maxPing->value && ping > sv_maxPing->value) {
                NET_OutOfBandPrint(NS_SERVER, from, "print\nServer is for low pings only\n");
                Com_DPrintf ("Client %i rejected on a too high ping\n", i);
                return;
            }

        }

    } else {
        // force the "ip" info key to "localhost"
        Info_SetValueForKey(userinfo, "ip", "localhost");
    }

    newcl = &temp;
    Com_Memset(newcl, 0, sizeof(client_t));

    // if there is already a slot for this ip, reuse it
    for (i = 0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

        if (cl->state == CS_FREE) {
            continue;
        }

        if (NET_CompareBaseAdr(from, cl->netchan.remoteAddress) && (cl->netchan.qport == qport || from.port == cl->netchan.remoteAddress.port)) {
            Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
            newcl = cl;
            goto gotnewcl;
        }
    }

    // find a client slot
    // if "sv_privateClients" is set > 0, then that number
    // of client slots will be reserved for connections that
    // have "password" set to the value of "sv_privatePassword"
    // Info requests will report the maxclients as if the private
    // slots didn't exist, to prevent people from trying to connect
    // to a full server.
    // This is to allow us to reserve a couple slots here on our
    // servers so we can play without having to kick people.

    // check for privateClient password
    password = Info_ValueForKey(userinfo, "password");
    if (!strcmp(password, sv_privatePassword->string)) {
        startIndex = 0;
    } else {
        // skip past the reserved slots
        startIndex = sv_privateClients->integer;
    }

    newcl = NULL;
    for (i = startIndex; i < sv_maxclients->integer ; i++) {
        cl = &svs.clients[i];
        if (cl->state == CS_FREE) {
            newcl = cl;
            break;
        }
    }

    if (!newcl) {

        if (NET_IsLocalAddress(from)) {

            count = 0;
            for (i = startIndex; i < sv_maxclients->integer ; i++) {
                cl = &svs.clients[i];
                if (cl->netchan.remoteAddress.type == NA_BOT) {
                    count++;
                }
            }

            // if they're all bots
            if (count >= sv_maxclients->integer - startIndex) {
                SV_DropClient(&svs.clients[sv_maxclients->integer - 1], "only bots on server");
                newcl = &svs.clients[sv_maxclients->integer - 1];
            }
            else {
                Com_Error(ERR_FATAL, "server is full on local connect\n");
                return;
            }
        }
        else {
            NET_OutOfBandPrint(NS_SERVER, from, "print\nServer is full\n");
            Com_DPrintf("Rejected a connection.\n");
            return;
        }

    }

    // we got a newcl, so reset the reliableSequence and reliableAcknowledge
    cl->reliableAcknowledge = 0;
    cl->reliableSequence = 0;

gotnewcl:
    // build a new connection
    // accept the new client
    // this is the only place a client_t is ever initialized
    *newcl = temp;
    clientNum = newcl - svs.clients;
    ent = SV_GentityNum(clientNum);
    newcl->gentity = ent;

    // save the challenge
    newcl->challenge = challenge;

    // save the address
    Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);
    // init the netchan queue
    newcl->netchan_end_queue = &newcl->netchan_start_queue;

    // clear server-side demo recording
    newcl->demo_recording = qfalse;
    newcl->demo_file = -1;
    newcl->demo_waiting = qfalse;
    newcl->demo_backoff = 1;
    newcl->demo_deltas = 0;

    // save the userinfo
    Q_strncpyz(newcl->userinfo, userinfo, sizeof(newcl->userinfo));

    // get the game a chance to reject this connection or modify the userinfo
    denied = VM_Call(gvm, GAME_CLIENT_CONNECT, clientNum, qtrue, qfalse); // firstTime = qtrue
    if (denied) {
        // we can't just use VM_ArgPtr, because that is only valid inside a VM_Call
        char *str = VM_ExplicitArgPtr(gvm, denied);
        NET_OutOfBandPrint(NS_SERVER, from, "print\n%s\n", str);
        Com_DPrintf ("Game rejected a connection: %s.\n", str);
        return;
    }

    SV_UserinfoChanged(newcl);

    // send the connect packet to the client
    NET_OutOfBandPrint(NS_SERVER, from, "connectResponse");

    Com_DPrintf("Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name);

    newcl->state = CS_CONNECTED;
    newcl->nextSnapshotTime = svs.time;
    newcl->lastPacketTime = svs.time;
    newcl->lastConnectTime = svs.time;
    newcl->numcmds = 0;

    // when we receive the first packet from the client, we will
    // notice that it is from a different serverid and that the
    // gamestate message was not just sent, forcing a retransmit
    newcl->gamestateMessageNum = -1;

    SV_LoadPositionFromFile(newcl, sv_mapname->string);

	// if this was the first client on the server, or the last client
    // the server can hold, send a heartbeat to the master.
    count = 0;
    for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
        if (svs.clients[i].state >= CS_CONNECTED) {
            count++;
        }
    }

    if (count == 1 || count == sv_maxclients->integer) {
        SV_Heartbeat_f();
    }

}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	challenge_t	*challenge;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	if (drop->netchan.remoteAddress.type != NA_BOT) {
		// see if we already have a challenge for this ip
		challenge = &svs.challenges[0];

		for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
			if ( NET_CompareAdr( drop->netchan.remoteAddress, challenge->adr ) ) {
				challenge->connected = qfalse;
				break;
			}
		}
	}

	// Kill any download
	SV_CloseDownload( drop );

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );

	if (drop->download)	{
		FS_FCloseFile( drop->download );
		drop->download = 0;
	}

	if (drop->demo_recording) {
	    // stop the server side demo if we were recording this client
       Cbuf_ExecuteText(EXEC_NOW, va("stopserverdemo %d", (int)(drop - svs.clients)));
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", reason);

	if ( drop->netchan.remoteAddress.type == NA_BOT ) {
		SV_BotFreeClient( drop - svs.clients );
	}
	
	// save the client position to a file
    SV_SavePositionToFile(drop, sv_mapname->string);

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );
	
	Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
	drop->state = CS_ZOMBIE;		// become free in a few seconds

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}

	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}

#ifdef USE_AUTH
/*
=====================
SV_Auth_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_Auth_DropClient( client_t *drop, const char *reason, const char *message ) {

	int		i;
	challenge_t	*challenge;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	if (drop->netchan.remoteAddress.type != NA_BOT) {
		// see if we already have a challenge for this ip
		challenge = &svs.challenges[0];

		for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
			if ( NET_CompareAdr( drop->netchan.remoteAddress, challenge->adr ) ) {
				challenge->connected = qfalse;
				break;
			}
		}
	}

	// Kill any download
	SV_CloseDownload( drop );

	// tell everyone why they got dropped
	if( strlen( reason ) > 0 ) SV_SendServerCommand( NULL, "print \"%s\n\"", reason );

	if (drop->download)	{
		FS_FCloseFile( drop->download );
		drop->download = 0;
	}

	if (drop->demo_recording) {
        // stop the server side demo if we were recording this client
       Cbuf_ExecuteText(EXEC_NOW, va("stopserverdemo %d", (int)(drop - svs.clients)));
    }

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", message);

	if ( drop->netchan.remoteAddress.type == NA_BOT ) {
		SV_BotFreeClient( drop - svs.clients );
	}
	SV_SavePositionToFile(drop, sv_mapname->string);
	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );
	
	Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
	drop->state = CS_ZOMBIE;		// become free in a few seconds

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}

	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}
#endif

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
void SV_SendClientGameState( client_t *client ) {
	int			start;
	entityState_t	*base, nullstate;
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

 	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	client->state = CS_PRIMED;
	client->pureAuthentic = 0;
	client->gotCP = qfalse;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( &msg, svc_configstring );
			MSG_WriteShort( &msg, start );
			MSG_WriteBigString( &msg, sv.configstrings[start] );
		}
	}

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, base, qtrue );
	}

	MSG_WriteByte( &msg, svc_EOF );

	MSG_WriteLong( &msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( &msg, sv.checksumFeed);

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}


/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->deltaMessage = -1;
	client->nextSnapshotTime = svs.time;	// generate a snapshot immediately

	if(cmd) {
		memcpy(&client->lastUsercmd, cmd, sizeof(client->lastUsercmd));
	} else {
		memset(&client->lastUsercmd, '\0', sizeof(client->lastUsercmd));
	}

	// call the game begin function
	VM_Call( gvm, GAME_CLIENT_BEGIN, client - svs.clients );
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free( cl->downloadBlocks[i] );
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
void SV_StopDownload_f( client_t *cl ) {
	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", (int) (cl - svs.clients), cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished
==================
*/
void SV_DoneDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;
	
	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", (int) (cl - svs.clients), block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", (int) (cl - svs.clients), cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
void SV_BeginDownload_f( client_t *cl ) {

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data 
==================
*/
void SV_WriteDownloadToClient( client_t *cl , msg_t *msg )
{
	int curindex;
	int rate;
	int blockspersnap;
	int idPack = 0, missionPack = 0, unreferenced = 1;
	char errorMessage[1024];
	char pakbuf[MAX_QPATH], *pakptr;
	int numRefPaks;

	if (!*cl->downloadName)
		return;	// Nothing being downloaded

	if (!cl->download) {
 		// Chop off filename extension.
		Com_sprintf(pakbuf, sizeof(pakbuf), "%s", cl->downloadName);
		pakptr = Q_strrchr(pakbuf, '.');
		
		if(pakptr)
		{
			*pakptr = '\0';

			// Check for pk3 filename extension
			if(!Q_stricmp(pakptr + 1, "pk3"))
			{
				const char *referencedPaks = FS_ReferencedPakNames();

				// Check whether the file appears in the list of referenced
				// paks to prevent downloading of arbitrary files.
				Cmd_TokenizeStringIgnoreQuotes(referencedPaks);
				numRefPaks = Cmd_Argc();

				for(curindex = 0; curindex < numRefPaks; curindex++)
				{
					if(!FS_FilenameCompare(Cmd_Argv(curindex), pakbuf))
					{
						unreferenced = 0;

						// now that we know the file is referenced,
						// check whether it's legal to download it.
						missionPack = FS_idPak(pakbuf, "missionpack");
						idPack = missionPack || FS_idPak(pakbuf, BASEGAME);

						break;
					}
				}
			}
		}

		// We open the file here
		if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
			(sv_allowDownload->integer & DLF_NO_UDP) ||
			idPack || unreferenced ||
			( cl->downloadSize = FS_SV_FOpenFileRead( cl->downloadName, &cl->download ) ) <= 0 ) {
			// cannot auto-download file
			if(unreferenced)
			{
				Com_Printf("clientDownload: %d : \"%s\" is not referenced and cannot be downloaded.\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" is not referenced and cannot be downloaded.", cl->downloadName);
			}
			else if (idPack) {
				Com_Printf("clientDownload: %d : \"%s\" cannot download id pk3 files\n", (int) (cl - svs.clients), cl->downloadName);
				if (missionPack) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload Team Arena file \"%s\"\n"
									"The Team Arena mission pack can be found in your local game store.", cl->downloadName);
				}
				else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload id pk3 file \"%s\"", cl->downloadName);
				}
			}
			else if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
				(sv_allowDownload->integer & DLF_NO_UDP) ) {

				Com_Printf("clientDownload: %d : \"%s\" download disabled", (int) (cl - svs.clients), cl->downloadName);
				if (sv_pure->integer) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
                    "The server you are connecting to is not a pure server, "
                    "set autodownload to No in your settings and you might be "
                    "able to join the game anyway.\n", cl->downloadName);
				}
			} else {
        // NOTE TTimo this is NOT supposed to happen unless bug in our filesystem scheme?
        //   if the pk3 is referenced, it must have been found somewhere in the filesystem
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;
			return;
		}
 
		Com_Printf( "clientDownload: %d : beginning \"%s\"\n", (int) (cl - svs.clients), cl->downloadName );
		
		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = Z_Malloc( MAX_DOWNLOAD_BLKSIZE );

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	// Loop up to window size times based on how many blocks we can fit in the
	// client snapMsec and rate

	// based on the rate, how many bytes can we fit in the snapMsec time of the client
	// normal rate / snapshotMsec calculation
	rate = cl->rate;
	if ( sv_maxRate->integer ) {
		if ( sv_maxRate->integer < 1000 ) {
			Cvar_Set( "sv_MaxRate", "1000" );
		}
		if ( sv_maxRate->integer < rate ) {
			rate = sv_maxRate->integer;
		}
	}
	if ( sv_minRate->integer ) {
		if ( sv_minRate->integer < 1000 )
			Cvar_Set( "sv_minRate", "1000" );
		if ( sv_minRate->integer > rate )
			rate = sv_minRate->integer;
	}

	if (!rate) {
		blockspersnap = 1;
	} else {
		blockspersnap = ( (rate * cl->snapshotMsec) / 1000 + MAX_DOWNLOAD_BLKSIZE ) /
			MAX_DOWNLOAD_BLKSIZE;
	}

	if (blockspersnap < 0)
		blockspersnap = 1;

	while (blockspersnap--) {

		// Write out the next section of the file, if we have already reached our window,
		// automatically start retransmitting

		if (cl->downloadClientBlock == cl->downloadCurrentBlock)
			return; // Nothing to transmit

		if (cl->downloadXmitBlock == cl->downloadCurrentBlock) {
			// We have transmitted the complete window, should we start resending?

			//FIXME:  This uses a hardcoded one second timeout for lost blocks
			//the timeout should be based on client rate somehow
			if (svs.time - cl->downloadSendTime > 1000)
				cl->downloadXmitBlock = cl->downloadClientBlock;
			else
				return;
		}

		// Send current block
		curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, cl->downloadXmitBlock );

		// block zero is special, contains file size
		if ( cl->downloadXmitBlock == 0 )
			MSG_WriteLong( msg, cl->downloadSize );
 
		MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

		// Write the block
		if ( cl->downloadBlockSize[curindex] ) {
			MSG_WriteData( msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex] );
		}

		Com_DPrintf( "clientDownload: %d : writing block %d\n", (int) (cl - svs.clients), cl->downloadXmitBlock );

		// Move on to the next block
		// It will get sent with next snap shot.  The rate will keep us in line.
		cl->downloadXmitBlock++;

		cl->downloadSendTime = svs.time;
	}
}

/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
static void SV_Disconnect_f( client_t *cl ) {
	SV_DropClient( cl, "disconnected" );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from 
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( sv_pure->integer != 0 ) {

		bGood = qtrue;
		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		bGood = (FS_FileIsInPAK("vm/cgame.qvm", &nChkSum1) == 1);
		if (bGood)
			bGood = (FS_FileIsInPAK("vm/ui.qvm", &nChkSum2) == 1);

		nClientPaks = Cmd_Argc();

		// start at arg 2 ( skip serverId cl_paks )
		nCurArg = 1;

		pArg = Cmd_Argv(nCurArg++);
		if(!pArg) {
			bGood = qfalse;
		}
		else
		{
			// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=475
			// we may get incoming cp sequences from a previous checksumFeed, which we need to ignore
			// since serverId is a frame count, it always goes up
			if (atoi(pArg) < sv.checksumFeedServerId)
			{
				Com_DPrintf("ignoring outdated cp command from client %s\n", cl->name);
				return;
			}
		}
	
		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		cl->gotCP = qtrue;

		if (bGood) {
			cl->pureAuthentic = 1;
		} 
		else {
			cl->pureAuthentic = 0;
			cl->nextSnapshotTime = -1;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure client detected. Invalid .PK3 files referenced!" );
		}
	}
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
	cl->gotCP = qfalse;
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val;
	char	*ip;
	int		i;
	int	len;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && com_dedicated->integer != 2 && sv_lanForceRate->integer == 1) {
		cl->rate = 99999;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (strlen(val)) {
			i = atoi(val);
			cl->rate = i;
			if (cl->rate < 1000) {
				cl->rate = 1000;
			} else if (cl->rate > 90000) {
				cl->rate = 90000;
			}
		} else {
			cl->rate = 3000;
		}
	}
	val = Info_ValueForKey (cl->userinfo, "handicap");
	if (strlen(val)) {
		i = atoi(val);
		if (i<=0 || i>100 || strlen(val) > 4) {
			Info_SetValueForKey( cl->userinfo, "handicap", "100" );
		}
	}

	// snaps command
	val = Info_ValueForKey (cl->userinfo, "snaps");
	if (strlen(val)) {
		i = atoi(val);
		if ( i < 1 ) {
			i = 1;
		} else if ( i > sv_fps->integer ) {
			i = sv_fps->integer;
		}
		cl->snapshotMsec = 1000/i;
	} else {
		cl->snapshotMsec = 50;
	}
	
	// TTimo
	// maintain the IP information
	// the banning code relies on this being consistently present
	if( NET_IsLocalAddress(cl->netchan.remoteAddress) )
		ip = "localhost";
	else
		ip = (char*)NET_AdrToString( cl->netchan.remoteAddress );

	val = Info_ValueForKey( cl->userinfo, "ip" );
	if( val[0] )
		len = strlen( ip ) - strlen( val ) + strlen( cl->userinfo );
	else
		len = strlen( ip ) + 4 + strlen( cl->userinfo );

	if( len >= MAX_INFO_STRING )
		SV_DropClient( cl, "userinfo string length exceeded" );
		
	else
		Info_SetValueForKey( cl->userinfo, "ip", ip );
	// get the client's cg_ghost value if we are in jump mode
    if (sv_gametype->integer == GT_JUMP) {
        cl->cm.ghost = SV_IsClientGhost(cl);
    }

}

/*
==================
SV_UpdateUserinfo_f
==================
*/
void SV_UpdateUserinfo_f( client_t *cl ) {
    
	if ( (sv_floodProtect->integer) && (cl->state >= CS_ACTIVE) && (svs.time < cl->nextReliableUserTime) ) {
		Q_strncpyz( cl->userinfobuffer, Cmd_Argv(1), sizeof(cl->userinfobuffer) );
		SV_SendServerCommand(cl, "print \"^7Command ^1delayed ^7due to sv_floodprotect!\n\"");
		return;
	}

    qboolean ghost = cl->cm.ghost; // Save here the current cg_ghost value in order to know after if it changed

	cl->userinfobuffer[0]=0;
	cl->nextReliableUserTime = svs.time + 5000;

	Q_strncpyz( cl->userinfo, Cmd_Argv(1), sizeof(cl->userinfo) );

	SV_UserinfoChanged(cl);

	// call prog code to allow overrides
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, cl - svs.clients );

    // get the client's cg_ghost value if we are in jump mode
    if (sv_gametype->integer == GT_JUMP) {
        cl->cm.ghost = SV_IsClientGhost(cl);
        // display ghost mode status if it changed
        if (ghost != cl->cm.ghost) {
            if (cl->cm.ghost) {
                SV_SendServerCommand(cl, "print \"^7Ghost Mode turned: [^2ON^7]\n\"");
            } else {
                SV_SendServerCommand(cl, "print \"^7Ghost Mode turned: [^1OFF^7]\n\"");
            }
        }
    }
}
//sysmyks mod


void checkGhostMode (void) {
	unsigned int time = Com_Milliseconds();
	int stopTime = time - sv.stopAntiBlockTime;
	if (stopTime > 0 && sv.checkGhostMode) {
		sv.checkGhostMode = qfalse;
		sv.stopAntiBlockTime = 0;
		Cmd_ExecuteString(va("mod_ghostPlayers 0"));
	}
}

/////////////////////////////////////////////////////////////////////
// Name        : SV_SavePosition_f
// Description : Saves clients current position
/////////////////////////////////////////////////////////////////////
static void SV_SavePosition_f(client_t *cl) {
    int             cid;
    playerState_t   *ps;
    
    // get the client slot
    cid = (int) (cl - svs.clients);
	
	if ( (cl->state >= CS_ACTIVE) && (svs.time < cl->nextReliableUserTime) ) {
		cl->numcmds = 1;
		//SV_SendServerCommand(cl, "print \"^7Command ^1delayed^7 due to sv_floodprotect.\"");
		return;
	}
	
	cl->nextReliableUserTime = svs.time + 1000;
	
    // if the server doesn't allow position save/load
    if (Cvar_VariableIntegerValue("g_allowPosSaving") < 1) {
        return;
    }
    
    // get the client playerState_t
    ps = SV_GameClientNum(cid);
    
    // save the position and the angles
    VectorCopy(ps->origin, cl->cm.savedPosition);
    VectorCopy(ps->viewangles, cl->cm.savedPositionAngle);
    
    // log command execution
    Com_Printf("ClientSavePosition: %d - %f - %f - %f\n",  cid, cl->cm.savedPosition[0],
                                                             cl->cm.savedPosition[1], cl->cm.savedPosition[2]);
    
    SV_SendServerCommand( cl, "print \"^7Your position has been saved^7\"");
}

/////////////////////////////////////////////////////////////////////
// SV_GhostThink
// Fixes the bugged cg_ghost in jump mode
/////////////////////////////////////////////////////////////////////
void SV_GhostThink(client_t *cl) {

    int               i;
    int               num;
    int               touch[MAX_GENTITIES];
    float             rad;
    vec3_t            mins, maxs;
    sharedEntity_t    *ent;
    sharedEntity_t    *oth;

    int cid;
    cid = cl - svs.clients;

    // if we are not playing jump mode
    if (sv_gametype->integer != GT_JUMP) {
        return;
    }

    // if the client is a spectator
    if (SV_GetClientTeam(cid) == TEAM_SPECTATOR) {
        return;
    }

    // if the client has cg_ghost disabled
    if (!cl->cm.ghost) {
        return;
    }

    // get the correspondent entity
    ent = SV_GentityNum(cid);
    rad = Com_Clamp(4.0, 1000.0, mod_ghostRadius->value);

    // calculate the box
    for (i = 0; i < 3; i++) {
        mins[i] = ent->r.currentOrigin[i] - rad;
        maxs[i] = ent->r.currentOrigin[i] + rad;
    }

    // get the entities the client is touching (the bounding box)
    num = SV_AreaEntities(mins, maxs, touch, MAX_GENTITIES);

    for (i = 0; i < num; i++) {

        // if the entity we are touching is not a client 
        if (touch[i] < 0 || touch[i] >= sv_maxclients->integer) {
            continue;
        }

        // get the touched entity
        oth = SV_GentityNum(touch[i]);

        // if the entity is the client itself
        if (ent->s.number == oth->s.number) {
            continue;
        }

        // set the content mask to both
        if (ent->r.contents & CONTENTS_BODY) {
            ent->r.contents &= ~CONTENTS_BODY;
            ent->r.contents |= CONTENTS_CORPSE;
        }
        if (oth->r.contents & CONTENTS_BODY) {
            oth->r.contents &= ~CONTENTS_BODY;
            oth->r.contents |= CONTENTS_CORPSE;
        }
    }
}

/////////////////////////////////////////////////////////////////////
// Name        : SV_LoadPosition_f
// Description : Loads clients saved position
/////////////////////////////////////////////////////////////////////
static void SV_LoadPosition_f(client_t *cl) {
    int             i, cid, angle;
    qboolean        jumprun;
    playerState_t   *ps;
    sharedEntity_t  *ent;
    static int      teleportAttempts = 0;
    static int      lastTeleportTime = 0;

    if (sv_gametype->integer != GT_JUMP) {
        return;
    }

    cid = (int)(cl - svs.clients);
    jumprun = cl->cm.ready;
    Com_Printf("JumpRun: %d \n", jumprun);

    if ((cl->state >= CS_ACTIVE) && (svs.time < cl->nextReliableUserTime)) {
        cl->numcmds = 1;
        return;
    }
    cl->nextReliableUserTime = svs.time + 12;

    if (Cvar_VariableIntegerValue("g_allowPosSaving") < 1) {
        return;
    }

    ent = SV_GentityNum(cid);
    ps = SV_GameClientNum(cid);

    if (!cl->cm.savedPosition[0] || !cl->cm.savedPosition[1] || !cl->cm.savedPosition[2]) {
        SV_SendServerCommand(cl, "print \"^7No position saved!\"");
        return;
    }

	int currentTime = svs.time;
    
	if (currentTime - lastTeleportTime > 2000) {
        teleportAttempts = 0;
    }

    if (jumprun) {
        teleportAttempts++;
        lastTeleportTime = currentTime;

        if (teleportAttempts < 3) {
            SV_SendServerCommand(cl, "print \"^3Warning:^7 Teleportation will cancel your run.^7\"");
            return;
        }

        Cmd_TokenizeString("ready");
        VM_Call(gvm, GAME_CLIENT_COMMAND, cl - svs.clients);
        teleportAttempts = 0;
    }

    VectorClear(ps->velocity);
    ps->stats[0] = 30000;

    VectorCopy(cl->cm.savedPosition, ps->origin);

    for (i = 0; i < 3; i++) {
        if (i == 0) {
            angle = ANGLE2SHORT(cl->cm.savedPositionAngle[i] - 11);
        } else {
            angle = ANGLE2SHORT(cl->cm.savedPositionAngle[i]);
        }
        ps->delta_angles[i] = angle - cl->lastUsercmd.angles[i];
    }

    VectorCopy(cl->cm.savedPositionAngle, ent->s.angles);
    VectorCopy(cl->cm.savedPositionAngle, ps->viewangles);

    if (jumprun) {
        Cmd_TokenizeString("ready");
        VM_Call(gvm, GAME_CLIENT_COMMAND, cl - svs.clients);
    }

    Com_Printf("ClientLoadPosition: %d - %f - %f - %f\n", cid, cl->cm.savedPosition[0],
                                                        cl->cm.savedPosition[1], cl->cm.savedPosition[2]);
    SV_SendServerCommand(cl, "print \"^7Your position has been loaded^7\"");
}

/////////////////////////////////////////////////////////////////////
// SV_ServerModInfo_f
/////////////////////////////////////////////////////////////////////
void SV_ServerModInfo_f(client_t* cl) {
    SV_SendServerCommand(cl, "chat \"^2==============================================================\"");
    SV_SendServerCommand(cl, "chat \"%s^7Server Version: %s\"", sv_tellprefix->string, Cvar_VariableString("version"));
    SV_SendServerCommand(cl, "chat \"%s^7Game Version: ^1Urban Terror %s\"", sv_tellprefix->string, Cvar_VariableString("g_modversion"));
    SV_SendServerCommand(cl, "chat \"^2==============================================================\"");
    SV_SendServerCommand(cl, "chat \"%s^7Credits: ^3Jump Titan Mod ^7by ^5Pedrxd ^7& ^5Th3K1ll3r\"", sv_tellprefix->string);
    SV_SendServerCommand(cl, "chat \"%s^7Modified by ^5Sysmyks \"", sv_tellprefix->string);
    SV_SendServerCommand(cl, "chat \"^2==============================================================\"");
}

/////////////////////////////////////////////////////////////////////
// SV_HelpCmdsList_f
/////////////////////////////////////////////////////////////////////
void SV_HelpCmdsList_f(client_t* cl) {

    char *version;
    char *jump = "Jump Mode";
    char *frag = "Frag Modes";

    if (!mod_enableHelpCmd->integer) {
        return;
    }

    if (sv_gametype->integer == GT_JUMP) {
        version = jump;
    } else {
        version = frag;
    }

    if (version == jump) {
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8-^7--------> Help Client - ^6List of Commands ^7[Jump Mode] <--------^8-\n\"");    
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8------------------------^7General Commands^8------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8-    ^1/help           ^8-     ^2/playerlist     ^8-    ^2/cg_rgb        ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-    ^2/reconnect      ^8-                     ^8-    ^2/r_gamma       ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-    ^2/disconnect     ^8-     ^1/modinfo      ^8-    ^2/sensitivity   ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-    ^2/connect        ^8-                     ^8-    ^2/bind          ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8--------------------------^7Game Commands^8-------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8-         ^2$location             ^8-       ^2/regainstamina         ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-         ^2/name                 ^8-       ^2/save                  ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-         ^2/kill                 ^8-       ^2/load                  ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-         ^2/ready ^5(RUN)          ^8-       ^2/allowgoto             ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-         ^2/cg_ghost ^5<0|1>       ^8-       ^2/goto                  ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8-              ^2!help       ^7[see commands spunky]               ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8-          ^2!register     ^7[register to see all orders]          ^8-\n\"");
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^2          [INFO] ^7Current Map:  [^3%s^7]            ^8-\n\"", sv_mapname->string);
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");

    } else if (version == frag) {
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8-^7--------> Help Client - ^6List of Commands ^7[Frag Mode] <--------^8-\n\"");    
        SV_SendServerCommand(cl, "print  \"^8----------------------------------------------------------------\n\"");
        SV_SendServerCommand(cl, "print  \"^8------------------------^7General Commands^8-----------------------\n\"");
        
    }
}

typedef struct {
	char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},
	{"save", SV_SavePosition_f},
    {"savepos", SV_SavePosition_f},
    {"load", SV_LoadPosition_f},
    {"loadpos", SV_LoadPosition_f},
	{"modinfo", SV_ServerModInfo_f},
    {"help", SV_HelpCmdsList_f},
	{NULL, NULL}
};
static ucmd_t ucmds_floodControl[] = {
    
    {NULL, NULL}
};

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {

	ucmd_t		*u;
	int			argsFromOneMaxlen;
	int			charCount;
	int			dollarCount;
	int			i;
	char		*arg;
	qboolean 	bProcessed = qfalse;
	qboolean 	exploitDetected = qfalse;
	
	Cmd_TokenizeString( s );

	// see if it is a server level command
	for (u=ucmds ; u->name ; u++) {
		if (!strcmp (Cmd_Argv(0), u->name) ) {
			u->func( cl );
			bProcessed = qtrue;
			break;
		}
	}

	if (clientOK) {

		// pass unknown strings to the game
		if ((!u->name) && (sv.state == SS_GAME) && (cl->state == CS_ACTIVE)) {
			Cmd_Args_Sanitize();

			argsFromOneMaxlen = -1;
			if (Q_stricmp("say", Cmd_Argv(0)) == 0 || Q_stricmp("say_team", Cmd_Argv(0)) == 0) {
				argsFromOneMaxlen = MAX_SAY_STRLEN;
			}
			else if (Q_stricmp("tell", Cmd_Argv(0)) == 0) {
				// A command will look like "tell 12 hi" or "tell foo hi".  The "12"
				// and "foo" in the examples will be counted towards MAX_SAY_STRLEN,
				// plus the space.
				argsFromOneMaxlen = MAX_SAY_STRLEN;
			}
			else if (Q_stricmp("ut_radio", Cmd_Argv(0)) == 0) {
				// We add 4 to this value because in a command such as
				// "ut_radio 1 1 affirmative", the args at indices 1 and 2 each
				// have length 1 and there is a space after them.
				argsFromOneMaxlen = MAX_RADIO_STRLEN + 4;
			}
			if (argsFromOneMaxlen >= 0) {
				charCount = 0;
				dollarCount = 0;
				for (i = Cmd_Argc() - 1; i >= 1; i--) {
					arg = Cmd_Argv(i);
					while (*arg) {
						if (++charCount > argsFromOneMaxlen) {
							exploitDetected = qtrue; break;
						}
						if (*arg == '$') {
							if (++dollarCount > MAX_DOLLAR_VARS) {
								exploitDetected = qtrue; break;
							}
							charCount += STRLEN_INCREMENT_PER_DOLLAR_VAR;
							if (charCount > argsFromOneMaxlen) {
								exploitDetected = qtrue; break;
							}
						}
						arg++;
					}
					if (exploitDetected) { break; }
					if (i != 1) { // Cmd_ArgsFrom() will add space
						if (++charCount > argsFromOneMaxlen) {
							exploitDetected = qtrue; break;
						}
					}
				}
			}
			if (exploitDetected) {
				Com_Printf("Buffer overflow exploit radio/say, possible attempt from %s\n", NET_AdrToString(cl->netchan.remoteAddress));
				SV_SendServerCommand(cl, "print \"Chat dropped due to message length constraints.\n\"");
				return;
			}

			VM_Call( gvm, GAME_CLIENT_COMMAND, cl - svs.clients );
		}
	}
    if (!bProcessed) {
        for (u = ucmds_floodControl; u->name; u++) {
            if (!strcmp(Cmd_Argv(0), u->name)) {
                // Vérifier le flood protect
                if ((sv_floodProtect->integer) && (cl->state >= CS_ACTIVE) && 
                    (svs.time < cl->nextReliableTime)) {
                    SV_SendServerCommand(cl, "print \"^7Command ^1delayed ^7due to sv_floodprotect!\n\"");
                    return;
                }
                u->func(cl);
                bProcessed = qtrue;
                break;
            }
        }
    }
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name, 
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players.  If we decide that we want to stall
	// the command, we will stop processing the rest of the packet,
	// including the usercmd.  This causes flooders to lag themselves
	// but not other people
	// We don't do this when the client hasn't been active yet since its
	// normal to spam a lot of commands when downloading
	if ( !com_cl_running->integer && 
		cl->state >= CS_ACTIVE &&
		sv_floodProtect->integer ) {
		if (svs.time < cl->nextReliableTime ) {
			if (++(cl->numcmds) > sv_floodProtect->integer ) {
				// ignore any other text messages from this client but let them keep playing
				// TTimo - moved the ignored verbose to the actual processing in SV_ExecuteClientCommand, only printing if the core doesn't intercept
				clientOk = qfalse;
			}
		} else {
			 cl->numcmds = 1;
		}
	} 

	// don't allow another command for one second
	cl->nextReliableTime = svs.time + 1000;

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	return qtrue;		// continue procesing
}



//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	// For the anti block
	if (sv.checkGhostMode && sv_ghostOnRoundstart->integer) {
		checkGhostMode();
	}
	SV_GhostThink(cl);
	VM_Call( gvm, GAME_CLIENT_THINK, cl - svs.clients );
}

/*
==================
SV_CalculateClientPing 
Calculates the client ping based on message acknowledgement time
==================
*/
static void SV_CalculateClientPing(client_t *client) {
    int i;
    int count;
    int totalPing;
    
    count = 0;
    totalPing = 0;

    // Get ping samples from frames
    for (i = 0; i < PACKET_BACKUP; i++) {
        if (client->frames[i].messageAcked <= 0) {
            continue;
        }
        
        // Calculate ping for this frame
        int ping = client->frames[i].messageAcked - client->frames[i].messageSent;
        
        // Ignore bogus values 
        if (ping < 0 || ping > 999) {
            continue;
        }
        
        totalPing += ping;
        count++;
    }

    // Calculate average ping
    if (count > 0) {
        client->ping = totalPing / count;
    } else {
        client->ping = 0;
    }
}

/*
==================
SV_UserMove

The message usually contains all the movement commands 
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
static void SV_UserMove( client_t *client, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;

	if ( delta ) {
		client->deltaMessage = client->messageAcknowledge;
	} else {
		client->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= client->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= Com_HashKey(client->reliableCommands[ client->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		oldcmd = cmd;
	}

	// save time for ping calculation
	client->frames[client->messageAcknowledge & PACKET_MASK].messageAcked = svs.time;
	// Calculer le ping
	SV_CalculateClientPing(client);
	// TTimo
	// catch the no-cp-yet situation before SV_ClientEnterWorld
	// if CS_ACTIVE, then it's time to trigger a new gamestate emission
	// if not, then we are getting remaining parasite usermove commands, which we should ignore
	if (sv_pure->integer != 0 && client->pureAuthentic == 0 && !client->gotCP) {
		if (client->state == CS_ACTIVE)
		{
			// we didn't get a cp yet, don't assume anything and just send the gamestate all over again
			Com_DPrintf( "%s: didn't get cp command, resending gamestate\n", client->name);
			SV_SendClientGameState( client );
		}
		return;
	}			
	
	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( client->state == CS_PRIMED ) {
		SV_ClientEnterWorld( client, &cmds[0] );
		// the moves can be processed normaly
	}
	
	// a bad cp command was sent, drop the client
	if (sv_pure->integer != 0 && client->pureAuthentic == 0) {		
		SV_DropClient( client, "Cannot validate pure client!");
		return;
	}

	if ( client->state != CS_ACTIVE ) {
		client->deltaMessage = -1;
		return;
	}

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= client->lastUsercmd.serverTime ) {
			continue;
		}
		SV_ClientThink (client, &cmds[ i ]);
	}
}


/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		return;
	}

	cl->reliableAcknowledge = MSG_ReadLong( msg );

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	if (cl->reliableAcknowledge < cl->reliableSequence - MAX_RELIABLE_COMMANDS) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	// 
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
	//
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=536
	// don't drop as long as previous command was a nextdl, after a dl is done, downloadName is set back to ""
	// but we still need to read the next message to move to next download or send gamestate
	// I don't like this hack though, it must have been working fine at some point, suspecting the fix is somewhere else
	if ( serverId != sv.serverId && !*cl->downloadName && !strstr(cl->lastClientCommandString, "nextdl") ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf("%s : ignoring pre map_restart / outdated client message\n", cl->name);
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		if ( cl->messageAcknowledge > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if( cl->oldServerTime && serverId == sv.serverId ){
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );
		if ( c == clc_EOF ) {
			break;
		}
		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );

	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", (int) (cl - svs.clients) );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}
