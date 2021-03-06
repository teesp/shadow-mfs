/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA.

   This file is part of MooseFS.

   MooseFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   MooseFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MooseFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>

#include "MFSCommunication.h"

#include "datapack.h"
#include "matocuserv.h"
#include "matocsserv.h"
#include "matomlserv.h"
#include "chunks.h"
#include "filesystem.h"
#include "random.h"
#include "acl.h"
#include "datacachemgr.h"
#include "charts.h"
#include "cfg.h"
#include "main.h"
#include "sockets.h"
#include "state.h"

#define MaxPacketSize 1000000

// chunklis.type
enum {FUSE_WRITE,FUSE_TRUNCATE};

#define NEWSESSION_TIMEOUT (7*86400)
#define OLDSESSION_TIMEOUT 7200

static session *sessionshead=NULL;
static serventry *matocuservhead=NULL;
static int lsock;
static int32_t lsockpdescpos;
static int exiting;
static int first_add_listen_sock;
extern int meta_ready;

// from config
static char *ListenHost;
static char *ListenPort;
static uint32_t RejectOld;
//static uint32_t Timeout;

/* new registration procedure */
session* matocuserv_new_session(uint8_t newsession,uint8_t nonewid) {
	session *asesdata;
	asesdata = (session*)malloc(sizeof(session));
	if (newsession==0 && nonewid) {
		asesdata->sessionid = 0;
	} else {
		asesdata->sessionid = fs_newsessionid();
	}
	asesdata->info = NULL;
	asesdata->peerip = 0;
	asesdata->sesflags = 0;
	asesdata->rootuid = 0;
	asesdata->rootgid = 0;
	asesdata->mapalluid = 0;
	asesdata->mapallgid = 0;
	asesdata->newsession = newsession;
	asesdata->rootinode = MFS_ROOT_ID;
	asesdata->openedfiles = NULL;
	asesdata->disconnected = 0;
	asesdata->nsocks = 1;
	memset(asesdata->currentopstats,0,4*16);
	memset(asesdata->lasthouropstats,0,4*16);
	asesdata->next = sessionshead;
	sessionshead = asesdata;
	return asesdata;
}

session* matocuserv_find_session(uint32_t sessionid) {
	session *asesdata;
	if (sessionid==0) {
		return NULL;
	}
	for (asesdata = sessionshead ; asesdata ; asesdata=asesdata->next) {
		if (asesdata->sessionid==sessionid) {
			asesdata->nsocks++;
			asesdata->disconnected = 0;
			return asesdata;
		}
	}
	return NULL;
}

void matocuserv_store_sessions() {
	session *asesdata;
	uint32_t ileng;
	uint8_t fsesrecord[161];	// 161 = 4+4+4+4+1+4+4+4+4+16*4+16*4
	uint8_t *ptr;
	int i;
	FILE *fd;

	fd = fopen("sessions.mfs.tmp","w");
	if (fd==NULL) {
		MFSLOG(LOG_WARNING,"can't store sessions, open error: %m");
		return;
	}
	if (fwrite("MFSS \001\006\001",8,1,fd)!=1) {
		MFSLOG(LOG_WARNING,"can't store sessions, fwrite error");
		fclose(fd);
		return;
	}

	for (asesdata = sessionshead ; asesdata ; asesdata=asesdata->next) {
		if (asesdata->newsession) {
			ptr = fsesrecord;
			if (asesdata->info) {
				ileng = strlen(asesdata->info);
			} else {
				ileng = 0;
			}
			put32bit(&ptr,asesdata->sessionid);
			put32bit(&ptr,ileng);
			put32bit(&ptr,asesdata->peerip);
			put32bit(&ptr,asesdata->rootinode);
			put8bit(&ptr,asesdata->sesflags);
			put32bit(&ptr,asesdata->rootuid);
			put32bit(&ptr,asesdata->rootgid);
			put32bit(&ptr,asesdata->mapalluid);
			put32bit(&ptr,asesdata->mapallgid);
			for (i=0 ; i<16 ; i++) {
				put32bit(&ptr,asesdata->currentopstats[i]);
			}
			for (i=0 ; i<16 ; i++) {
				put32bit(&ptr,asesdata->lasthouropstats[i]);
			}
			if (fwrite(fsesrecord,161,1,fd)!=1) {
				MFSLOG(LOG_WARNING,"can't store sessions, fwrite error");
				fclose(fd);
				return;
			}
			if (ileng>0) {
				if (fwrite(asesdata->info,ileng,1,fd)!=1) {
					MFSLOG(LOG_WARNING,"can't store sessions, fwrite error");
					fclose(fd);
					return;
				}
			}
		}
	}
	if (fclose(fd)!=0) {
		MFSLOG(LOG_WARNING,"can't store sessions, fclose error: %m");
		return;
	}
	if (rename("sessions.mfs.tmp","sessions.mfs")<0) {
		MFSLOG(LOG_WARNING,"can't store sessions, rename error: %m");
	}
}

int matocuserv_load_sessions() {
	session *asesdata;
	uint32_t ileng;
	uint8_t fsesrecord[161];	// 153 = 4+4+4+4+1+4+4+4+4+16*4+16*4
	const uint8_t *ptr;
	int i,imp15;
	int r;
	FILE *fd;

	if(isslave()) {
		MFSLOG(LOG_NOTICE, "slave should not load session\n");
		return 0;
	}

	fd = fopen("sessions.mfs","r");
	if (fd==NULL) {
		MFSLOG(LOG_WARNING,"can't load sessions, fopen error: %m");
		if (errno==ENOENT) {	// it's ok if file does not exist
			return 0;
		} else {
			return -1;
		}
	}
	if (fread(fsesrecord,8,1,fd)!=1) {
		MFSLOG(LOG_WARNING,"can't load sessions, fread error");
		fclose(fd);
		return -1;
	}
	if (memcmp(fsesrecord,"MFSS 1.5",8)==0) {
		imp15=1;
	} else if (memcmp(fsesrecord,"MFSS \001\006\001",8)==0) {
		imp15=0;
	} else {
		MFSLOG(LOG_WARNING,"can't load sessions, bad header");
		fclose(fd);
		return -1;
	}

	while (!feof(fd)) {
		if (imp15) {
			r = fread(fsesrecord,153,1,fd);
		} else {
			r = fread(fsesrecord,161,1,fd);
		}
		if (r==1) {
			ptr = fsesrecord;
			asesdata = (session*)malloc(sizeof(session));
			asesdata->sessionid = get32bit(&ptr);
			ileng = get32bit(&ptr);
			asesdata->peerip = get32bit(&ptr);
			asesdata->rootinode = get32bit(&ptr);
			asesdata->sesflags = get8bit(&ptr);
			asesdata->rootuid = get32bit(&ptr);
			asesdata->rootgid = get32bit(&ptr);
			if (imp15) {
				asesdata->mapalluid = 0;
				asesdata->mapallgid = 0;
			} else {
				asesdata->mapalluid = get32bit(&ptr);
				asesdata->mapallgid = get32bit(&ptr);
			}
			asesdata->info = NULL;
			asesdata->newsession = 1;
			asesdata->openedfiles = NULL;
			asesdata->disconnected = get_current_time();
			asesdata->nsocks = 0;
			for (i=0 ; i<16 ; i++) {
				asesdata->currentopstats[i] = get32bit(&ptr);
			}
			for (i=0 ; i<16 ; i++) {
				asesdata->lasthouropstats[i] = get32bit(&ptr);
			}
			if (ileng>0) {
				asesdata->info = malloc(ileng+1);
				if (fread(asesdata->info,ileng,1,fd)!=1) {
					free(asesdata->info);
					free(asesdata);
					MFSLOG(LOG_WARNING,"can't load sessions, fread error");
					fclose(fd);
					return -1;
				}
				asesdata->info[ileng]=0;
			}
			asesdata->next = sessionshead;
			sessionshead = asesdata;
		}
		if (ferror(fd)) {
			MFSLOG(LOG_WARNING,"can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
	}
	MFSLOG(LOG_NOTICE,"sessions have been loaded");
	fclose(fd);
	return 0;
}

/* old registration procedure */
/*
session* matocuserv_get_session(uint32_t sessionid) {
	// if sessionid==0 - create new record with next id
	session *asesdata;

	if (sessionid>0) {
		for (asesdata = sessionshead ; asesdata ; asesdata=asesdata->next) {
			if (asesdata->sessionid==sessionid) {
				asesdata->nsocks++;
				asesdata->disconnected = 0;
				return asesdata;
			}
		}
	}
	asesdata = (session*)malloc(sizeof(session));
	if (sessionid==0) {
		asesdata->sessionid = fs_newsessionid();
	} else {
		asesdata->sessionid = sessionid;
	}
	asesdata->openedfiles = NULL;
	asesdata->disconnected = 0;
	asesdata->nsocks = 1;
	memset(asesdata->currentopstats,0,4*16);
	memset(asesdata->lasthouropstats,0,4*16);
	asesdata->next = sessionshead;
	sessionshead = asesdata;
	return asesdata;
}
*/

int matocuserv_insert_openfile(session* cr,uint32_t inode) {
	filelist *ofptr,**ofpptr;
	int status;

	ofpptr = &(cr->openedfiles);
	while ((ofptr=*ofpptr)) {
		if (ofptr->inode==inode) {
			return STATUS_OK;	// file already aquired - nothing to do
		}
		if (ofptr->inode>inode) {
			break;
		}
		ofpptr = &(ofptr->next);
	}
	status = fs_aquire(inode,cr->sessionid);
	if (status==STATUS_OK) {
		ofptr = (filelist*)malloc(sizeof(filelist));
		ofptr->inode = inode;
		ofptr->next = *ofpptr;
		*ofpptr = ofptr;
	}
	return status;
}

void matocuserv_init_sessions(uint32_t sessionid,uint32_t inode) {
	session *asesdata;
	filelist *ofptr,**ofpptr;

	for (asesdata = sessionshead ; asesdata && asesdata->sessionid!=sessionid; asesdata=asesdata->next) ;
	if (asesdata==NULL) {
		asesdata = (session*)malloc(sizeof(session));
		asesdata->sessionid = sessionid;
/* session created by filesystem - only for old clients (pre 1.5.13) */
		asesdata->info = NULL;
		asesdata->peerip = 0;
		asesdata->sesflags = 0;
		asesdata->rootuid = 0;
		asesdata->rootgid = 0;
		asesdata->mapalluid = 0;
		asesdata->mapallgid = 0;
		asesdata->newsession = 0;
		asesdata->rootinode = MFS_ROOT_ID;
		asesdata->openedfiles = NULL;
		asesdata->disconnected = get_current_time();
		asesdata->nsocks = 0;
		memset(asesdata->currentopstats,0,4*16);
		memset(asesdata->lasthouropstats,0,4*16);
		asesdata->next = sessionshead;
		sessionshead = asesdata;
	}

	ofpptr = &(asesdata->openedfiles);
	while ((ofptr=*ofpptr)) {
		if (ofptr->inode==inode) {
			return;
		}
		if (ofptr->inode>inode) {
			break;
		}
		ofpptr = &(ofptr->next);
	}
	ofptr = (filelist*)malloc(sizeof(filelist));
	ofptr->inode = inode;
	ofptr->next = *ofpptr;
	*ofpptr = ofptr;
}

uint8_t* matocuserv_createpacket(serventry *eptr,uint32_t type,uint32_t size) {
	packetstruct *outpacket;
	uint8_t *ptr;
	uint32_t psize;

	outpacket=(packetstruct*)malloc(sizeof(packetstruct));
	if (outpacket==NULL) {
		return NULL;
	}
	psize = size+8;
	outpacket->packet=malloc(psize);
	outpacket->bytesleft = psize;
	if (outpacket->packet==NULL) {
		free(outpacket);
		return NULL;
	}
	ptr = outpacket->packet;
	put32bit(&ptr,type);
	put32bit(&ptr,size);
	outpacket->startptr = (uint8_t*)(outpacket->packet);
	outpacket->next = NULL;
	*(eptr->outputtail) = outpacket;
	eptr->outputtail = &(outpacket->next);
	return ptr;
}

/*
int matocuserv_open_check(serventry *eptr,uint32_t fid) {
	filelist *fl;
	for (fl=eptr->openedfiles ; fl ; fl=fl->next) {
		if (fl->fid==fid) {
			return 0;
		}
	}
	return -1;
}
*/

void matocuserv_chunk_status(uint64_t chunkid,uint8_t status) {
	uint32_t qid,inode,uid,gid,auid,agid;
	uint64_t fleng;
	uint8_t type,attr[35];
	uint32_t version;
//	uint8_t rstat;
//	uint32_t ip;
//	uint16_t port;
	uint8_t *ptr;
	uint8_t count;
	uint8_t loc[100*6];
	chunklist *cl,**acl;
	serventry *eptr,*eaptr;

	eptr=NULL;
	qid=0;
	fleng=0;
	type=0;
	inode=0;
	uid=0;
	gid=0;
	auid=0;
	agid=0;
	for (eaptr = matocuservhead ; eaptr && eptr==NULL ; eaptr=eaptr->next) {
		if (eaptr->mode!=KILL && eaptr->listen_sock==0) {
			acl = &(eaptr->chunkdelayedops);
			while (*acl && eptr==NULL) {
				cl = *acl;
				if (cl->chunkid==chunkid) {
					eptr = eaptr;
					qid = cl->qid;
					fleng = cl->fleng;
					type = cl->type;
					inode = cl->inode;
					uid = cl->uid;
					gid = cl->gid;
					auid = cl->auid;
					agid = cl->agid;

					*acl = cl->next;
					free(cl);
				} else {
					acl = &(cl->next);
				}
			}
		}
	}

	if (!eptr) {
		MFSLOG(LOG_WARNING,"got chunk status, but don't want it");
		return;
	}
	if (status==STATUS_OK) {
		dcm_modify(inode,eptr->sesdata->sessionid);
	}
	switch (type) {
	case FUSE_WRITE:
		if (status==STATUS_OK) {
			status=chunk_getversionandlocations(chunkid,eptr->peerip,&version,&count,loc);
			//syslog(LOG_NOTICE,"get version for chunk %"PRIu64" -> %"PRIu32,chunkid,version);
		}
		if (status!=STATUS_OK) {
			ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_WRITE_CHUNK,5);
			if (ptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			put32bit(&ptr,qid);
			put8bit(&ptr,status);
			fs_writeend(0,0,chunkid);	// ignore status - just do it.
			return;
		}
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_WRITE_CHUNK,24+count*6);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,qid);
		put64bit(&ptr,fleng);
		put64bit(&ptr,chunkid);
		put32bit(&ptr,version);
		memcpy(ptr,loc,count*6);
//		for (i=0 ; i<count ; i++) {
//			if (matocsserv_getlocation(sptr[i],&ip,&port)<0) {
//				put32bit(&ptr,0);
//				put16bit(&ptr,0);
//			} else {
//				put32bit(&ptr,ip);
//				put16bit(&ptr,port);
//			}
//		}
		return;
	case FUSE_TRUNCATE:
		fs_end_setlength(chunkid);

		if (status!=STATUS_OK) {
			ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_TRUNCATE,5);
			if (ptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			put32bit(&ptr,qid);
			put8bit(&ptr,status);
			return;
		}
		fs_do_setlength(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,auid,agid,fleng,attr);
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_TRUNCATE,39);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,qid);
		memcpy(ptr,attr,35);
		return;
	default:
		MFSLOG(LOG_WARNING,"got chunk status, but operation type is unknown");
	}
}

void matocuserv_cserv_list(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_CSERV_LIST - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_CSERV_LIST,matocsserv_cservlist_size());
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	matocsserv_cservlist_data(ptr);
}

void matocuserv_session_list(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	serventry *eaptr;
	uint32_t size,ileng,pleng,i;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_SESSION_LIST - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	size = 0;
	for (eaptr = matocuservhead ; eaptr ; eaptr=eaptr->next) {
		if (eaptr->mode!=KILL && eaptr->sesdata && eaptr->registered>0 && eaptr->registered<100 && eaptr->listen_sock==0) {
			size += 165;
			if (eaptr->sesdata->info) {
				size += strlen(eaptr->sesdata->info);
			}
			if (eaptr->sesdata->rootinode==0) {
				size += 1;
			} else {
				size += fs_getdirpath_size(eaptr->sesdata->rootinode);
			}
		}
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_SESSION_LIST,size);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	for (eaptr = matocuservhead ; eaptr ; eaptr=eaptr->next) {
		if (eaptr->mode!=KILL && eaptr->sesdata && eaptr->registered>0 && eaptr->registered<100 && eaptr->listen_sock==0) {
//			tcpgetpeer(eaptr->sock,&ip,NULL);
			put32bit(&ptr,eaptr->sesdata->sessionid);
			put32bit(&ptr,eaptr->peerip);
			put32bit(&ptr,eaptr->version);
			if (eaptr->sesdata->info) {
				ileng = strlen(eaptr->sesdata->info);
				put32bit(&ptr,ileng);
				memcpy(ptr,eaptr->sesdata->info,ileng);
				ptr+=ileng;
			} else {
				put32bit(&ptr,0);
			}
			if (eaptr->sesdata->rootinode==0) {
				put32bit(&ptr,1);
				put8bit(&ptr,'.');
			} else {
				pleng = fs_getdirpath_size(eaptr->sesdata->rootinode);
				put32bit(&ptr,pleng);
				if (pleng>0) {
					fs_getdirpath_data(eaptr->sesdata->rootinode,ptr,pleng);
					ptr+=pleng;
				}
			}
			put8bit(&ptr,eaptr->sesdata->sesflags);
			put32bit(&ptr,eaptr->sesdata->rootuid);
			put32bit(&ptr,eaptr->sesdata->rootgid);
			put32bit(&ptr,eaptr->sesdata->mapalluid);
			put32bit(&ptr,eaptr->sesdata->mapallgid);
			if (eaptr->sesdata) {
				for (i=0 ; i<16 ; i++) {
					put32bit(&ptr,eaptr->sesdata->currentopstats[i]);
				}
				for (i=0 ; i<16 ; i++) {
					put32bit(&ptr,eaptr->sesdata->lasthouropstats[i]);
				}
			} else {
				memset(ptr,0xFF,8*16);
				ptr+=8*16;
			}
		}
	}
}

void matocuserv_chart(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t chartid;
	uint8_t *ptr;
	uint32_t l;

	if (length!=4) {
		MFSLOG(LOG_NOTICE,"CUTOAN_CHART - wrong size (%"PRIu32"/4)",length);
		eptr->mode = KILL;
		return;
	}
	chartid = get32bit(&data);
	l = charts_make_png(chartid);
	ptr = matocuserv_createpacket(eptr,ANTOCU_CHART,l);
	if (ptr==NULL) {
		eptr->mode=KILL;
		return;
	}
	if (l>0) {
		charts_get_png(ptr);
	}
}

void matocuserv_chart_data(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t chartid;
	uint8_t *ptr;
	uint32_t l;

	if (length!=4) {
		MFSLOG(LOG_NOTICE,"CUTOAN_CHART_DATA - wrong size (%"PRIu32"/4)",length);
		eptr->mode = KILL;
		return;
	}
	chartid = get32bit(&data);
	l = charts_datasize(chartid);
	ptr = matocuserv_createpacket(eptr,ANTOCU_CHART_DATA,l);
	if (ptr==NULL) {
		eptr->mode=KILL;
		return;
	}
	if (l>0) {
		charts_makedata(ptr,chartid);
	}
}

void matocuserv_info(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t totalspace,availspace,trspace,respace;
	uint32_t trnodes,renodes,inodes,dnodes,fnodes;
	uint32_t chunks,chunkcopies,tdcopies;
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_INFO - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	fs_info(&totalspace,&availspace,&trspace,&trnodes,&respace,&renodes,&inodes,&dnodes,&fnodes);
	chunk_info(&chunks,&chunkcopies,&tdcopies);
	ptr = matocuserv_createpacket(eptr,MATOCU_INFO,68);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	/* put32bit(&buff,VERSION): */
	put16bit(&ptr,VERSMAJ);
	put8bit(&ptr,VERSMID);
	put8bit(&ptr,VERSMIN);
	/* --- */
	put64bit(&ptr,totalspace);
	put64bit(&ptr,availspace);
	put64bit(&ptr,trspace);
	put32bit(&ptr,trnodes);
	put64bit(&ptr,respace);
	put32bit(&ptr,renodes);
	put32bit(&ptr,inodes);
	put32bit(&ptr,dnodes);
	put32bit(&ptr,fnodes);
	put32bit(&ptr,chunks);
	put32bit(&ptr,chunkcopies);
	put32bit(&ptr,tdcopies);
}

void matocuserv_fstest_info(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t loopstart,loopend,files,ugfiles,mfiles,chunks,ugchunks,mchunks,msgbuffleng;
	char *msgbuff;
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FSTEST_INFO - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	fs_test_getdata(&loopstart,&loopend,&files,&ugfiles,&mfiles,&chunks,&ugchunks,&mchunks,&msgbuff,&msgbuffleng);
	ptr = matocuserv_createpacket(eptr,MATOCU_FSTEST_INFO,msgbuffleng+36);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,loopstart);
	put32bit(&ptr,loopend);
	put32bit(&ptr,files);
	put32bit(&ptr,ugfiles);
	put32bit(&ptr,mfiles);
	put32bit(&ptr,chunks);
	put32bit(&ptr,ugchunks);
	put32bit(&ptr,mchunks);
	put32bit(&ptr,msgbuffleng);
	if (msgbuffleng>0) {
		memcpy(ptr,msgbuff,msgbuffleng);
	}
}

void matocuserv_chunkstest_info(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_CHUNKSTEST_INFO - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_CHUNKSTEST_INFO,52);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	chunk_store_info(ptr);
}

void matocuserv_chunks_matrix(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	uint8_t matrixid;
	(void)data;
	if (length>1) {
		MFSLOG(LOG_NOTICE,"CUTOMA_CHUNKS_MATRIX - wrong size (%"PRIu32"/0|1)",length);
		eptr->mode = KILL;
		return;
	}
	if (length==1) {
		matrixid = get8bit(&data);
	} else {
		matrixid = 0;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_CHUNKS_MATRIX,484);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	chunk_store_chunkcounters(ptr,matrixid);
}

void matocuserv_quota_info(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_QUOTA_INFO - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_QUOTA_INFO,fs_getquotainfo_size());
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	fs_getquotainfo_data(ptr);
}

void matocuserv_exports_info(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_EXPORTS_INFO - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_EXPORTS_INFO,acl_info_size());
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	acl_info_data(ptr);
}

void matocuserv_mlog_list(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_MLOG_LIST - wrong size (%"PRIu32"/0)",length);
		eptr->mode = KILL;
		return;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_MLOG_LIST,matomlserv_mloglist_size());
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	matomlserv_mloglist_data(ptr);
}

void matocuserv_fuse_register(serventry *eptr,const uint8_t *data,uint32_t length) {
	const uint8_t *rptr;
	uint8_t *wptr;
	uint32_t sessionid;
	uint8_t status;
	uint8_t tools;

      if(isslave()) {
           return;
      }	

	if (length<64) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER - wrong size (%"PRIu32"/<65)",length);
		eptr->mode = KILL;
		return;
	}
	tools = (memcmp(data,FUSE_REGISTER_BLOB_TOOLS_NOACL,64)==0)?1:0;
	if (memcmp(data,FUSE_REGISTER_BLOB_NOACL,64)==0 || tools) {
		if (RejectOld) {
			MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/NOACL - rejected (option REJECT_OLD_CLIENTS is set)");
			eptr->mode = KILL;
			return;
		}
		if (tools) {
			if (length!=64 && length!=68) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/NOACL-TOOLS - wrong size (%"PRIu32"/64|68)",length);
				eptr->mode = KILL;
				return;
			}
		} else {
			if (length!=68 && length!=72) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/NOACL-MOUNT - wrong size (%"PRIu32"/68|72)",length);
				eptr->mode = KILL;
				return;
			}
		}
		rptr = data+64;
		if (tools) {
			sessionid = 0;
			if (length==68) {
				eptr->version = get32bit(&rptr);
			}
		} else {
			sessionid = get32bit(&rptr);
			if (length==72) {
				eptr->version = get32bit(&rptr);
			}
		}
		if (eptr->version<0x010500 && !tools) {
			MFSLOG(LOG_NOTICE,"got register packet from mount older than 1.5 - rejecting");
			eptr->mode = KILL;
			return;
		}
		if (sessionid==0) {	// new session
			status = STATUS_OK; // acl_check(eptr->peerip,(const uint8_t*)"",NULL,NULL,&sesflags);	// check privileges for '/' w/o password
//			if (status==STATUS_OK) {
				eptr->sesdata = matocuserv_new_session(0,tools);
				if (eptr->sesdata==NULL) {
					MFSLOG(LOG_NOTICE,"can't allocate session record");
					eptr->mode = KILL;
					return;
				}
				eptr->sesdata->rootinode = MFS_ROOT_ID;
				eptr->sesdata->sesflags = 0;
				eptr->sesdata->peerip = eptr->peerip;
//			}
		} else { // reconnect or tools
			eptr->sesdata = matocuserv_find_session(sessionid);
			if (eptr->sesdata==NULL) {	// in old model if session doesn't exist then create it
				eptr->sesdata = matocuserv_new_session(0,0);
				if (eptr->sesdata==NULL) {
					MFSLOG(LOG_NOTICE,"can't allocate session record");
					eptr->mode = KILL;
					return;
				}
				eptr->sesdata->rootinode = MFS_ROOT_ID;
				eptr->sesdata->sesflags = 0;
				eptr->sesdata->peerip = eptr->peerip;
				status = STATUS_OK;
			} else if (eptr->sesdata->peerip==0) { // created by "filesystem"
				eptr->sesdata->peerip = eptr->peerip;
				status = STATUS_OK;
			} else if (eptr->sesdata->peerip==eptr->peerip) {
				status = STATUS_OK;
			} else {
				status = ERROR_EACCES;
			}
		}
		if (tools) {
			wptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REGISTER,1);
		} else {
			wptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REGISTER,(status!=STATUS_OK)?1:4);
		}
		if (wptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		if (status!=STATUS_OK) {
			put8bit(&wptr,status);
			return;
		}
		if (tools) {
			put8bit(&wptr,status);
		} else {
			sessionid = eptr->sesdata->sessionid;
			put32bit(&wptr,sessionid);
		}
		eptr->registered = (tools)?100:1;
		return;
	} else if (memcmp(data,FUSE_REGISTER_BLOB_ACL,64)==0) {
		uint32_t rootinode;
		uint8_t sesflags;
		uint32_t rootuid,rootgid;
		uint32_t mapalluid,mapallgid;
		uint32_t ileng,pleng;
		uint8_t i,rcode;
		const uint8_t *path;
		const char *info;

		if (length<65) {
			MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL - wrong size (%"PRIu32"/<65)",length);
			eptr->mode = KILL;
			return;
		}

		rptr = data+64;
		rcode = get8bit(&rptr);

		switch (rcode) {
		case 1:
			if (length!=65) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.1 - wrong size (%"PRIu32"/65)",length);
				eptr->mode = KILL;
				return;
			}
			wptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REGISTER,32);
			if (wptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			for (i=0 ; i<32 ; i++) {
				eptr->passwordrnd[i]=rndu8();
			}
			memcpy(wptr,eptr->passwordrnd,32);
			return;
		case 2:
			if (length<77) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.2 - wrong size (%"PRIu32"/>=77)",length);
				eptr->mode = KILL;
				return;
			}
			eptr->version = get32bit(&rptr);
			ileng = get32bit(&rptr);
			if (length<77+ileng) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.2 - wrong size (%"PRIu32"/>=77+ileng(%"PRIu32"))",length,ileng);
				eptr->mode = KILL;
				return;
			}
			info = (const char*)rptr;
			rptr+=ileng;
			pleng = get32bit(&rptr);
			if (length!=77+ileng+pleng && length!=77+16+ileng+pleng) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.2 - wrong size (%"PRIu32"/77+ileng(%"PRIu32")+pleng(%"PRIu32")[+16])",length,ileng,pleng);
				eptr->mode = KILL;
				return;
			}
			path = rptr;
			rptr+=pleng;
			if (pleng>0 && rptr[-1]!=0) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.2 - received path without ending zero");
				eptr->mode = KILL;
				return;
			}
			if (pleng==0) {
				path = (const uint8_t*)"";
			}
			if (length==77+16+ileng+pleng) {
				status = acl_check(eptr->peerip,eptr->version,0,path,eptr->passwordrnd,rptr,&sesflags,&rootuid,&rootgid,&mapalluid,&mapallgid);
			} else {
				status = acl_check(eptr->peerip,eptr->version,0,path,NULL,NULL,&sesflags,&rootuid,&rootgid,&mapalluid,&mapallgid);
			}
			if (status==STATUS_OK) {
				status = fs_getrootinode(&rootinode,path);
			}
			if (status==STATUS_OK) {
				eptr->sesdata = matocuserv_new_session(1,0);
				if (eptr->sesdata==NULL) {
					MFSLOG(LOG_NOTICE,"can't allocate session record");
					eptr->mode = KILL;
					return;
				}
				eptr->sesdata->rootinode = rootinode;
				eptr->sesdata->sesflags = sesflags;
				eptr->sesdata->rootuid = rootuid;
				eptr->sesdata->rootgid = rootgid;
				eptr->sesdata->mapalluid = mapalluid;
				eptr->sesdata->mapallgid = mapallgid;
				eptr->sesdata->peerip = eptr->peerip;
				if (ileng>0) {
					if (info[ileng-1]==0) {
						eptr->sesdata->info = strdup(info);
					} else {
						eptr->sesdata->info = malloc(ileng+1);
						memcpy(eptr->sesdata->info,info,ileng);
						eptr->sesdata->info[ileng]=0;
					}
				}
				matocuserv_store_sessions();
			}
			wptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REGISTER,(status==STATUS_OK)?((eptr->version>=0x010601)?21:13):1);
			if (wptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			if (status!=STATUS_OK) {
				put8bit(&wptr,status);
				return;
			}
			sessionid = eptr->sesdata->sessionid;
			put32bit(&wptr,sessionid);
			put8bit(&wptr,sesflags);
			put32bit(&wptr,rootuid);
			put32bit(&wptr,rootgid);
			if (eptr->version>=0x010601) {
				put32bit(&wptr,mapalluid);
				put32bit(&wptr,mapallgid);
			}
			eptr->registered = 1;
			return;
		case 5:
			if (length<73) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.5 - wrong size (%"PRIu32"/>=73)",length);
				eptr->mode = KILL;
				return;
			}
			eptr->version = get32bit(&rptr);
			ileng = get32bit(&rptr);
			if (length!=73+ileng && length!=73+16+ileng) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.5 - wrong size (%"PRIu32"/73+ileng(%"PRIu32")[+16])",length,ileng);
				eptr->mode = KILL;
				return;
			}
			info = (const char*)rptr;
			rptr+=ileng;
			if (length==73+16+ileng) {
				status = acl_check(eptr->peerip,eptr->version,1,NULL,eptr->passwordrnd,rptr,&sesflags,&rootuid,&rootgid,&mapalluid,&mapallgid);
			} else {
				status = acl_check(eptr->peerip,eptr->version,1,NULL,NULL,NULL,&sesflags,&rootuid,&rootgid,&mapalluid,&mapallgid);
			}
			if (status==STATUS_OK) {
				eptr->sesdata = matocuserv_new_session(1,0);
				if (eptr->sesdata==NULL) {
					MFSLOG(LOG_NOTICE,"can't allocate session record");
					eptr->mode = KILL;
					return;
				}
				eptr->sesdata->rootinode = 0;
				eptr->sesdata->sesflags = sesflags;
				eptr->sesdata->rootuid = 0;
				eptr->sesdata->rootgid = 0;
				eptr->sesdata->mapalluid = 0;
				eptr->sesdata->mapallgid = 0;
				eptr->sesdata->peerip = eptr->peerip;
				if (ileng>0) {
					if (info[ileng-1]==0) {
						eptr->sesdata->info = strdup(info);
					} else {
						eptr->sesdata->info = malloc(ileng+1);
						memcpy(eptr->sesdata->info,info,ileng);
						eptr->sesdata->info[ileng]=0;
					}
				}
				matocuserv_store_sessions();
			}
			wptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REGISTER,(status==STATUS_OK)?5:1);
			if (wptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			if (status!=STATUS_OK) {
				put8bit(&wptr,status);
				return;
			}
			sessionid = eptr->sesdata->sessionid;
			put32bit(&wptr,sessionid);
			put8bit(&wptr,sesflags);
			eptr->registered = 1;
			return;
		case 3:
		case 4:
			if (length<73) {
				MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL.%"PRIu8" - wrong size (%"PRIu32"/73)",rcode,length);
				eptr->mode = KILL;
				return;
			}
			sessionid = get32bit(&rptr);
			eptr->version = get32bit(&rptr);
			eptr->sesdata = matocuserv_find_session(sessionid);
			if (eptr->sesdata==NULL) {
				status = ERROR_BADSESSIONID;
			} else {
				if ((eptr->sesdata->sesflags&SESFLAG_DYNAMICIP)==0 && eptr->peerip!=eptr->sesdata->peerip) {
					status = ERROR_EACCES;
				} else {
					status = STATUS_OK;
				}
			}
			wptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REGISTER,1);
			if (wptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			put8bit(&wptr,status);
			if (status!=STATUS_OK) {
				return;
			}
			eptr->registered = (rcode==3)?1:100;
			return;
		}
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER/ACL - wrong rcode (%"PRIu8")",rcode);
		eptr->mode = KILL;
		return;
	} else {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REGISTER - wrong register blob");
		eptr->mode = KILL;
		return;
	}
}

void matocuserv_fuse_reserved_inodes(serventry *eptr,const uint8_t *data,uint32_t length) {
	const uint8_t *ptr;
	filelist *ofptr,**ofpptr;
	uint32_t inode;

	if ((length&0x3)!=0) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_RESERVED_INODES - wrong size (%"PRIu32"/N*4)",length);
		eptr->mode = KILL;
		return;
	}

	ptr = data;
//	endptr = ptr + length;
	ofpptr = &(eptr->sesdata->openedfiles);
	length >>= 2;
	if (length) {
		length--;
		inode = get32bit(&ptr);
	} else {
		inode=0;
	}

	while ((ofptr=*ofpptr) && inode>0) {
		if (ofptr->inode<inode) {
			fs_release(ofptr->inode,eptr->sesdata->sessionid);
			*ofpptr = ofptr->next;
			free(ofptr);
		} else if (ofptr->inode>inode) {
			if (fs_aquire(inode,eptr->sesdata->sessionid)==STATUS_OK) {
				ofptr = (filelist*)malloc(sizeof(filelist));
				ofptr->next = *ofpptr;
				ofptr->inode = inode;
				*ofpptr = ofptr;
				ofpptr = &(ofptr->next);
			}
			if (length) {
				length--;
				inode = get32bit(&ptr);
			} else {
				inode=0;
			}
		} else {
			ofpptr = &(ofptr->next);
			if (length) {
				length--;
				inode = get32bit(&ptr);
			} else {
				inode=0;
			}
		}
	}
	while (inode>0) {
		if (fs_aquire(inode,eptr->sesdata->sessionid)==STATUS_OK) {
			ofptr = (filelist*)malloc(sizeof(filelist));
			ofptr->next = *ofpptr;
			ofptr->inode = inode;
			*ofpptr = ofptr;
			ofpptr = &(ofptr->next);
		}
		if (length) {
			length--;
			inode = get32bit(&ptr);
		} else {
			inode=0;
		}
	}
	while ((ofptr=*ofpptr)) {
		fs_release(ofptr->inode,eptr->sesdata->sessionid);
		*ofpptr = ofptr->next;
		free(ofptr);
	}

}

static inline void matocuserv_ugid_remap(serventry *eptr,uint32_t *auid,uint32_t *agid) {
	if (*auid==0) {
		*auid = eptr->sesdata->rootuid;
		if (agid) {
			*agid = eptr->sesdata->rootgid;
		}
	} else if (eptr->sesdata->sesflags&SESFLAG_MAPALL) {
		*auid = eptr->sesdata->mapalluid;
		if (agid) {
			*agid = eptr->sesdata->mapallgid;
		}
	}
}

/*
static inline void matocuserv_ugid_attr_remap(serventry *eptr,uint8_t attr[35],uint32_t auid,uint32_t agid) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t fuid,fgid;
	if (auid!=0 && (eptr->sesdata->sesflags&SESFLAG_MAPALL)) {
		rptr = attr+3;
		fuid = get32bit(&rptr);
		fgid = get32bit(&rptr);
		fuid = (fuid==eptr->sesdata->mapalluid)?auid:0;
		fgid = (fgid==eptr->sesdata->mapallgid)?agid:0;
		wptr = attr+3;
		put32bit(&wptr,fuid);
		put32bit(&wptr,fgid);
	}
}
*/
void matocuserv_fuse_statfs(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t totalspace,availspace,trashspace,reservedspace;
	uint32_t msgid,inodes;
	uint8_t *ptr;
	if (length!=4) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_STATFS - wrong size (%"PRIu32"/4)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	fs_statfs(eptr->sesdata->rootinode,eptr->sesdata->sesflags,&totalspace,&availspace,&trashspace,&reservedspace,&inodes);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_STATFS,40);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put64bit(&ptr,totalspace);
	put64bit(&ptr,availspace);
	put64bit(&ptr,trashspace);
	put64bit(&ptr,reservedspace);
	put32bit(&ptr,inodes);
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[0]++;
	}
}

void matocuserv_fuse_access(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid;
	uint8_t modemask;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_ACCESS - wrong size (%"PRIu32"/17)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	modemask = get8bit(&data);
	status = fs_access(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,modemask);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_ACCESS,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}

void matocuserv_fuse_lookup(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint8_t nleng;
	const uint8_t *name;
	uint32_t newinode;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_LOOKUP - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	nleng = get8bit(&data);
	if (length!=17U+nleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_LOOKUP - wrong size (%"PRIu32":nleng=%"PRIu8")",length,nleng);
		eptr->mode = KILL;
		return;
	}
	name = data;
	data += nleng;
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_lookup(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,nleng,name,uid,gid,auid,agid,&newinode,attr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_LOOKUP,(status!=STATUS_OK)?5:43);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,newinode);
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[3]++;
	}
}

void matocuserv_fuse_getattr(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8 && length!=16) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETATTR - wrong size (%"PRIu32"/8,16)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	if (length==16) {
		auid = uid = get32bit(&data);
		agid = gid = get32bit(&data);
		matocuserv_ugid_remap(eptr,&uid,&gid);
	} else {
		auid = uid = 12345;
		agid = gid = 12345;
	}
	status = fs_getattr(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,auid,agid,attr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETATTR,(status!=STATUS_OK)?5:39);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[1]++;
	}
}

void matocuserv_fuse_setattr(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint16_t setmask;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint16_t attrmode;
	uint32_t attruid,attrgid,attratime,attrmtime;
	if (length!=35) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SETATTR - wrong size (%"PRIu32"/35)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	setmask = get8bit(&data);
	attrmode = get16bit(&data);
	attruid = get32bit(&data);
	attrgid = get32bit(&data);
	attratime = get32bit(&data);
	attrmtime = get32bit(&data);
	if (setmask&(SET_GOAL_FLAG|SET_LENGTH_FLAG|SET_OPENED_FLAG)) {
		status = ERROR_EINVAL;
	} else {
		status = fs_setattr(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,auid,agid,setmask,attrmode,attruid,attrgid,attratime,attrmtime,attr);
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SETATTR,(status!=STATUS_OK)?5:39);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[2]++;
	}
}

void matocuserv_fuse_truncate(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t opened;
	uint8_t status;
	uint64_t attrlength;
	chunklist *cl;
	uint64_t chunkid;
	if (length!=24 && length!=25) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_TRUNCATE - wrong size (%"PRIu32"/24|25)",length);
		eptr->mode = KILL;
		return;
	}
	opened = 0;
	msgid = get32bit(&data);
	inode = get32bit(&data);
	if (length==25) {
		opened = get8bit(&data);
	}
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	if (length==24) {
		if (uid==0 && gid!=0) {	// stupid "opened" patch for old clients
			opened = 1;
		}
	}
	matocuserv_ugid_remap(eptr,&uid,&gid);
	attrlength = get64bit(&data);
	status = fs_try_setlength(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,opened,uid,gid,auid,agid,attrlength,attr,&chunkid);
	if (status==ERROR_DELAYED) {
		cl = (chunklist*)malloc(sizeof(chunklist));
		cl->chunkid = chunkid;
		cl->qid = msgid;
		cl->inode = inode;
		cl->uid = uid;
		cl->gid = gid;
		cl->auid = auid;
		cl->agid = agid;
		cl->fleng = attrlength;
		cl->type = FUSE_TRUNCATE;
		cl->next = eptr->chunkdelayedops;
		eptr->chunkdelayedops = cl;
		if (eptr->sesdata) {
			eptr->sesdata->currentopstats[2]++;
		}
		return;
	}
	if (status==STATUS_OK) {
		status = fs_do_setlength(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,auid,agid,attrlength,attr);
	}
	if (status==STATUS_OK) {
		dcm_modify(inode,eptr->sesdata->sessionid);
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_TRUNCATE,(status!=STATUS_OK)?5:39);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[2]++;
	}
}

void matocuserv_fuse_readlink(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint32_t pleng;
	uint8_t *path;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_READLINK - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_readlink(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,&pleng,&path);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_READLINK,(status!=STATUS_OK)?5:8+pleng+1);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,pleng+1);
		if (pleng>0) {
			memcpy(ptr,path,pleng);
		}
		ptr[pleng]=0;
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[7]++;
	}
}

void matocuserv_fuse_symlink(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint8_t nleng;
	const uint8_t *name,*path;
	uint32_t uid,gid,auid,agid;
	uint32_t pleng;
	uint32_t newinode;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;
	if (length<21) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SYMLINK - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	nleng = get8bit(&data);
	if (length<21U+nleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SYMLINK - wrong size (%"PRIu32":nleng=%"PRIu8")",length,nleng);
		eptr->mode = KILL;
		return;
	}
	name = data;
	data += nleng;
	pleng = get32bit(&data);
	if (length!=21U+nleng+pleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SYMLINK - wrong size (%"PRIu32":nleng=%"PRIu8":pleng=%"PRIu32")",length,nleng,pleng);
		eptr->mode = KILL;
		return;
	}
	path = data;
	data += pleng;
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	while (pleng>0 && path[pleng-1]==0) {
		pleng--;
	}
	status = fs_symlink(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,nleng,name,pleng,path,uid,gid,auid,agid,&newinode,attr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SYMLINK,(status!=STATUS_OK)?5:43);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,newinode);
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[6]++;
	}
}

void matocuserv_fuse_mknod(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid,rdev;
	uint8_t nleng;
	const uint8_t *name;
	uint8_t type;
	uint16_t mode;
	uint32_t newinode;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<24) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_MKNOD - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	nleng = get8bit(&data);
	if (length!=24U+nleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_MKNOD - wrong size (%"PRIu32":nleng=%"PRIu8")",length,nleng);
		eptr->mode = KILL;
		return;
	}
	name = data;
	data += nleng;
	type = get8bit(&data);
	mode = get16bit(&data);
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	rdev = get32bit(&data);
	status = fs_mknod(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,nleng,name,type,mode,uid,gid,auid,agid,rdev,&newinode,attr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_MKNOD,(status!=STATUS_OK)?5:43);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,newinode);
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[8]++;
	}
}

void matocuserv_fuse_mkdir(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint8_t nleng;
	const uint8_t *name;
	uint16_t mode;
	uint32_t newinode;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<19) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_MKDIR - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	nleng = get8bit(&data);
	if (length!=19U+nleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_MKDIR - wrong size (%"PRIu32":nleng=%"PRIu8")",length,nleng);
		eptr->mode = KILL;
		return;
	}
	name = data;
	data += nleng;
	mode = get16bit(&data);
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_mkdir(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,nleng,name,mode,uid,gid,auid,agid,&newinode,attr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_MKDIR,(status!=STATUS_OK)?5:43);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,newinode);
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[4]++;
	}
}

void matocuserv_fuse_unlink(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid;
	uint8_t nleng;
	const uint8_t *name;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_UNLINK - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	nleng = get8bit(&data);
	if (length!=17U+nleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_UNLINK - wrong size (%"PRIu32":nleng=%"PRIu8")",length,nleng);
		eptr->mode = KILL;
		return;
	}
	name = data;
	data += nleng;
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_unlink(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,nleng,name,uid,gid);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_UNLINK,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[9]++;
	}
}

void matocuserv_fuse_rmdir(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid;
	uint8_t nleng;
	const uint8_t *name;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_RMDIR - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	nleng = get8bit(&data);
	if (length!=17U+nleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_RMDIR - wrong size (%"PRIu32":nleng=%"PRIu8")",length,nleng);
		eptr->mode = KILL;
		return;
	}
	name = data;
	data += nleng;
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_rmdir(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,nleng,name,uid,gid);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_RMDIR,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[5]++;
	}
}

void matocuserv_fuse_rename(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode_src,inode_dst;
	uint8_t nleng_src,nleng_dst;
	const uint8_t *name_src,*name_dst;
	uint32_t uid,gid;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;
	if (length<22) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_RENAME - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode_src = get32bit(&data);
	nleng_src = get8bit(&data);
	if (length<22U+nleng_src) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_RENAME - wrong size (%"PRIu32":nleng_src=%"PRIu8")",length,nleng_src);
		eptr->mode = KILL;
		return;
	}
	name_src = data;
	data += nleng_src;
	inode_dst = get32bit(&data);
	nleng_dst = get8bit(&data);
	if (length!=22U+nleng_src+nleng_dst) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_RENAME - wrong size (%"PRIu32":nleng_src=%"PRIu8":nleng_dst=%"PRIu8")",length,nleng_src,nleng_dst);
		eptr->mode = KILL;
		return;
	}
	name_dst = data;
	data += nleng_dst;
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_rename(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode_src,nleng_src,name_src,inode_dst,nleng_dst,name_dst,uid,gid);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_RENAME,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[10]++;
	}
}

// na razie funkcja zostanie wy��czona - do momentu zaimplementowania posiksowego twardego linka
void matocuserv_fuse_link(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,inode_dst;
	uint8_t nleng_dst;
	const uint8_t *name_dst;
	uint32_t uid,gid,auid,agid;
	uint32_t newinode;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<21) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_LINK - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	inode_dst = get32bit(&data);
	nleng_dst = get8bit(&data);
	if (length!=21U+nleng_dst) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_LINK - wrong size (%"PRIu32":nleng_dst=%"PRIu8")",length,nleng_dst);
		eptr->mode = KILL;
		return;
	}
	name_dst = data;
	data += nleng_dst;
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_link(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,inode_dst,nleng_dst,name_dst,uid,gid,auid,agid,&newinode,attr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_LINK,(status!=STATUS_OK)?5:43);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,newinode);
		memcpy(ptr,attr,35);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[11]++;
	}
}

void matocuserv_fuse_getdir(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint8_t flags;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint32_t dleng;
	void *custom;
	if (length!=16 && length!=17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETDIR - wrong size (%"PRIu32"/16|17)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	if (length==17) {
		flags = get8bit(&data);
	} else {
		flags = 0;
	}
	status = fs_readdir_size(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,flags,&custom,&dleng);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETDIR,(status!=STATUS_OK)?5:4+dleng);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		fs_readdir_data(eptr->sesdata->rootinode,eptr->sesdata->sesflags,uid,gid,auid,agid,flags,custom,ptr);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[12]++;
	}
}

void matocuserv_fuse_open(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid,auid,agid;
	uint8_t flags;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	int allowcache;
	if (length!=17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_OPEN - wrong size (%"PRIu32"/17)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	auid = uid = get32bit(&data);
	agid = gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	flags = get8bit(&data);
	status = matocuserv_insert_openfile(eptr->sesdata,inode);
	if (status==STATUS_OK) {
		status = fs_opencheck(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,auid,agid,flags,attr);
	}
	if (eptr->version>=0x010609 && status==STATUS_OK) {
		allowcache = dcm_open(inode,eptr->sesdata->sessionid);
		if (allowcache==0) {
			attr[1]&=(0xFF^(MATTR_ALLOWDATACACHE<<4));
		}
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_OPEN,39);
	} else {
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_OPEN,5);
	}
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (eptr->version>=0x010609 && status==STATUS_OK) {
		memcpy(ptr,attr,35);
	} else {
		put8bit(&ptr,status);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[13]++;
	}
}

void matocuserv_fuse_read_chunk(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	uint8_t status;
	uint32_t inode;
	uint32_t indx;
	uint64_t chunkid;
	uint64_t fleng;
	uint32_t version;
//	uint32_t ip;
//	uint16_t port;
	uint8_t count;
	uint8_t loc[100*6];
	uint32_t msgid;
	if (length!=12) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_READ_CHUNK - wrong size (%"PRIu32"/12)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	indx = get32bit(&data);
//	if (matocuserv_open_check(eptr,inode)<0) {
//		status = ERROR_NOTOPENED;
//	} else {
		status = fs_readchunk(inode,indx,&chunkid,&fleng);
//	}
	if (status==STATUS_OK) {
		if (chunkid>0) {
			status = chunk_getversionandlocations(chunkid,eptr->peerip,&version,&count,loc);
		} else {
			version = 0;
			count = 0;
		}
	}
	if (status!=STATUS_OK) {
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_READ_CHUNK,5);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,msgid);
		put8bit(&ptr,status);
		return;
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_READ_CHUNK,24+count*6);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put64bit(&ptr,fleng);
	put64bit(&ptr,chunkid);
	put32bit(&ptr,version);
	memcpy(ptr,loc,count*6);
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[14]++;
	}
}

void matocuserv_fuse_write_chunk(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	uint8_t status;
	uint32_t inode;
	uint32_t indx;
	uint64_t fleng;
	uint64_t chunkid;
	uint32_t msgid;
	uint8_t opflag;
	chunklist *cl;
	uint32_t version;
	uint8_t count;
	uint8_t loc[100*6];

	if (length!=12) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_WRITE_CHUNK - wrong size (%"PRIu32"/12)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	indx = get32bit(&data);
	if (eptr->sesdata->sesflags&SESFLAG_READONLY) {
		status = ERROR_EROFS;
	} else {
		status = fs_writechunk(inode,indx,&chunkid,&fleng,&opflag);
	}
	if (status!=STATUS_OK) {
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_WRITE_CHUNK,5);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,msgid);
		put8bit(&ptr,status);
		return;
	}
	if (opflag) {	// wait for operation end
		cl = (chunklist*)malloc(sizeof(chunklist));
		cl->inode = inode;
		cl->chunkid = chunkid;
		cl->qid = msgid;
		cl->fleng = fleng;
		cl->type = FUSE_WRITE;
		cl->next = eptr->chunkdelayedops;
		eptr->chunkdelayedops = cl;
	} else {	// return status immediately
		dcm_modify(inode,eptr->sesdata->sessionid);
		status=chunk_getversionandlocations(chunkid,eptr->peerip,&version,&count,loc);
		if (status!=STATUS_OK) {
			ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_WRITE_CHUNK,5);
			if (ptr==NULL) {
				MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
				eptr->mode = KILL;
				return;
			}
			put32bit(&ptr,msgid);
			put8bit(&ptr,status);
			fs_writeend(0,0,chunkid);	// ignore status - just do it.
			return;
		}
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_WRITE_CHUNK,24+count*6);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,msgid);
		put64bit(&ptr,fleng);
		put64bit(&ptr,chunkid);
		put32bit(&ptr,version);
		memcpy(ptr,loc,count*6);
	}
	if (eptr->sesdata) {
		eptr->sesdata->currentopstats[15]++;
	}
}

void matocuserv_fuse_write_chunk_end(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	uint32_t msgid;
	uint32_t inode;
	uint64_t fleng;
	uint64_t chunkid;
	uint8_t status;
//	chunklist *cl,**acl;
	if (length!=24) {
		MFSLOG(LOG_NOTICE,"CUTOMA_WRITE_CHUNK_END - wrong size (%"PRIu32"/24)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	chunkid = get64bit(&data);
	inode = get32bit(&data);
	fleng = get64bit(&data);
	if (eptr->sesdata->sesflags&SESFLAG_READONLY) {
		status = ERROR_EROFS;
	} else {
		status = fs_writeend(inode,fleng,chunkid);
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_WRITE_CHUNK_END,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}

void matocuserv_fuse_repair(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,gid;
	uint32_t msgid;
	uint32_t chunksnotchanged,chunkserased,chunksrepaired;
	uint8_t *ptr;
	uint8_t status;
	if (length!=16) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_REPAIR - wrong size (%"PRIu32"/16)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_repair(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,gid,&chunksnotchanged,&chunkserased,&chunksrepaired);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_REPAIR,(status!=STATUS_OK)?5:16);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=0) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,chunksnotchanged);
		put32bit(&ptr,chunkserased);
		put32bit(&ptr,chunksrepaired);
	}
}

void matocuserv_fuse_check(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint16_t t16,chunkcount[256];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_CHECK - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_checkfile(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,chunkcount);
	if (status!=STATUS_OK) {
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_CHECK,5);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,msgid);
		put8bit(&ptr,status);
	} else {
		uint32_t i,j;
		j=0;
		for (i=0 ; i<256 ; i++) {
			if (chunkcount[i]>0) {
				j++;
			}
		}
		ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_CHECK,4+j*3);
		if (ptr==NULL) {
			MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
			eptr->mode = KILL;
			return;
		}
		put32bit(&ptr,msgid);
		for (i=0 ; i<256 ; i++) {
			t16 = chunkcount[i];
			if (t16>0) {
				put8bit(&ptr,i);
				put16bit(&ptr,t16);
			}
		}
	}
}


void matocuserv_fuse_gettrashtime(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint8_t gmode;
	void *fptr,*dptr;
	uint32_t fnodes,dnodes;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=9) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETTRASHTIME - wrong size (%"PRIu32"/9)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	gmode = get8bit(&data);
	status = fs_gettrashtime_prepare(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,gmode,&fptr,&dptr,&fnodes,&dnodes);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETTRASHTIME,(status!=STATUS_OK)?5:12+8*(fnodes+dnodes));
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,fnodes);
		put32bit(&ptr,dnodes);
		fs_gettrashtime_store(fptr,dptr,ptr);
	}
}

void matocuserv_fuse_settrashtime(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid,trashtime;
	uint32_t msgid;
	uint8_t smode;
	uint32_t changed,notchanged,notpermitted;
	uint8_t *ptr;
	uint8_t status;
	if (length!=17) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SETTRASHTIME - wrong size (%"PRIu32"/17)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	uid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,NULL);
	trashtime = get32bit(&data);
	smode = get8bit(&data);
	status = fs_settrashtime(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,trashtime,smode,&changed,&notchanged,&notpermitted);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SETTRASHTIME,(status!=STATUS_OK)?5:16);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,changed);
		put32bit(&ptr,notchanged);
		put32bit(&ptr,notpermitted);
	}
}

void matocuserv_fuse_getgoal(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint32_t msgid;
	uint32_t fgtab[10],dgtab[10];
	uint8_t i,fn,dn,gmode;
	uint8_t *ptr;
	uint8_t status;
	if (length!=9) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETGOAL - wrong size (%"PRIu32"/9)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	gmode = get8bit(&data);
	status = fs_getgoal(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,gmode,fgtab,dgtab);
	fn=0;
	dn=0;
	if (status==STATUS_OK) {
		for (i=1 ; i<10 ; i++) {
			if (fgtab[i]) {
				fn++;
			}
			if (dgtab[i]) {
				dn++;
			}
		}
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETGOAL,(status!=STATUS_OK)?5:6+5*(fn+dn));
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put8bit(&ptr,fn);
		put8bit(&ptr,dn);
		for (i=1 ; i<10 ; i++) {
			if (fgtab[i]) {
				put8bit(&ptr,i);
				put32bit(&ptr,fgtab[i]);
			}
		}
		for (i=1 ; i<10 ; i++) {
			if (dgtab[i]) {
				put8bit(&ptr,i);
				put32bit(&ptr,dgtab[i]);
			}
		}
	}
}

void matocuserv_fuse_setgoal(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid;
	uint32_t msgid;
	uint8_t goal,smode;
	uint32_t changed,notchanged,notpermitted;
	uint8_t *ptr;
	uint8_t status;
	if (length!=14) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SETGOAL - wrong size (%"PRIu32"/14)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	uid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,NULL);
	goal = get8bit(&data);
	smode = get8bit(&data);
	status = fs_setgoal(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,goal,smode,&changed,&notchanged,&notpermitted);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SETGOAL,(status!=STATUS_OK)?5:16);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,changed);
		put32bit(&ptr,notchanged);
		put32bit(&ptr,notpermitted);
	}
}

void matocuserv_fuse_geteattr(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint32_t msgid;
	uint32_t feattrtab[16],deattrtab[16];
	uint8_t i,fn,dn,gmode;
	uint8_t *ptr;
	uint8_t status;
	if (length!=9) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETEATTR - wrong size (%"PRIu32"/9)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	gmode = get8bit(&data);
	status = fs_geteattr(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,gmode,feattrtab,deattrtab);
	fn=0;
	dn=0;
	if (status==STATUS_OK) {
		for (i=0 ; i<16 ; i++) {
			if (feattrtab[i]) {
				fn++;
			}
			if (deattrtab[i]) {
				dn++;
			}
		}
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETEATTR,(status!=STATUS_OK)?5:6+5*(fn+dn));
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put8bit(&ptr,fn);
		put8bit(&ptr,dn);
		for (i=0 ; i<16 ; i++) {
			if (feattrtab[i]) {
				put8bit(&ptr,i);
				put32bit(&ptr,feattrtab[i]);
			}
		}
		for (i=0 ; i<16 ; i++) {
			if (deattrtab[i]) {
				put8bit(&ptr,i);
				put32bit(&ptr,deattrtab[i]);
			}
		}
	}
}

void matocuserv_fuse_seteattr(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,uid;
	uint32_t msgid;
	uint8_t eattr,smode;
	uint32_t changed,notchanged,notpermitted;
	uint8_t *ptr;
	uint8_t status;
	if (length!=14) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SETEATTR - wrong size (%"PRIu32"/14)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	uid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,NULL);
	eattr = get8bit(&data);
	smode = get8bit(&data);
	status = fs_seteattr(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,eattr,smode,&changed,&notchanged,&notpermitted);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SETEATTR,(status!=STATUS_OK)?5:16);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,changed);
		put32bit(&ptr,notchanged);
		put32bit(&ptr,notpermitted);
	}
}

void matocuserv_fuse_append(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,inode_src,uid,gid;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=20) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_APPEND - wrong size (%"PRIu32"/20)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	inode_src = get32bit(&data);
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	status = fs_append(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,inode_src,uid,gid);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_APPEND,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}

void matocuserv_fuse_snapshot(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,inode_dst;
	uint8_t nleng_dst;
	const uint8_t *name_dst;
	uint32_t uid,gid;
	uint8_t canoverwrite;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length<22) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SNAPSHOT - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	inode_dst = get32bit(&data);
	nleng_dst = get8bit(&data);
	if (length!=22U+nleng_dst) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SNAPSHOT - wrong size (%"PRIu32":nleng_dst=%"PRIu8")",length,nleng_dst);
		eptr->mode = KILL;
		return;
	}
	name_dst = data;
	data += nleng_dst;
	uid = get32bit(&data);
	gid = get32bit(&data);
	matocuserv_ugid_remap(eptr,&uid,&gid);
	canoverwrite = get8bit(&data);
	status = fs_snapshot(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,inode_dst,nleng_dst,name_dst,uid,gid,canoverwrite);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SNAPSHOT,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}

void matocuserv_fuse_quotacontrol(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t flags,del;
	uint32_t sinodes,hinodes,curinodes;
	uint64_t slength,ssize,srealsize,hlength,hsize,hrealsize,curlength,cursize,currealsize;
	uint32_t msgid,inode;
	uint8_t *ptr;
	uint8_t status;
	if (length!=65 && length!=9) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_QUOTACONTROL - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	flags = get8bit(&data);
	if (length==65) {
		sinodes = get32bit(&data);
		slength = get64bit(&data);
		ssize = get64bit(&data);
		srealsize = get64bit(&data);
		hinodes = get32bit(&data);
		hlength = get64bit(&data);
		hsize = get64bit(&data);
		hrealsize = get64bit(&data);
		del=0;
	} else {
		del=1;
	}
	if (flags && eptr->sesdata->rootuid!=0) {
		status = ERROR_EACCES;
	} else {
		status = fs_quotacontrol(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,del,&flags,&sinodes,&slength,&ssize,&srealsize,&hinodes,&hlength,&hsize,&hrealsize,&curinodes,&curlength,&cursize,&currealsize);
	}
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_QUOTACONTROL,(status!=STATUS_OK)?5:89);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put8bit(&ptr,flags);
		put32bit(&ptr,sinodes);
		put64bit(&ptr,slength);
		put64bit(&ptr,ssize);
		put64bit(&ptr,srealsize);
		put32bit(&ptr,hinodes);
		put64bit(&ptr,hlength);
		put64bit(&ptr,hsize);
		put64bit(&ptr,hrealsize);
		put32bit(&ptr,curinodes);
		put64bit(&ptr,curlength);
		put64bit(&ptr,cursize);
		put64bit(&ptr,currealsize);
	}
}

/*
void matocuserv_fuse_eattr(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t mode,eattr,fneattr;
	uint32_t msgid,inode,uid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=14) {
		syslog(LOG_NOTICE,"CUTOMA_FUSE_EATTR - wrong size (%"PRIu32")",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	uid = get32bit(&data);
	mode = get8bit(&data);
	eattr = get8bit(&data);
	status = fs_eattr(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,uid,mode,&eattr,&fneattr);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_EATTR,(status!=STATUS_OK)?5:6);
	if (ptr==NULL) {
		syslog(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put8bit(&ptr,eattr);
		put8bit(&ptr,fneattr);
	}
}
*/

void matocuserv_fuse_getdirstats_old(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,inodes,files,dirs,chunks;
	uint64_t leng,size,rsize;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETDIRSTATS - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_get_dir_stats(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,&inodes,&dirs,&files,&chunks,&leng,&size,&rsize);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETDIRSTATS,(status!=STATUS_OK)?5:60);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,inodes);
		put32bit(&ptr,dirs);
		put32bit(&ptr,files);
		put32bit(&ptr,0);
		put32bit(&ptr,0);
		put32bit(&ptr,chunks);
		put32bit(&ptr,0);
		put32bit(&ptr,0);
		put64bit(&ptr,leng);
		put64bit(&ptr,size);
		put64bit(&ptr,rsize);
	}
}

void matocuserv_fuse_getdirstats(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode,inodes,files,dirs,chunks;
	uint64_t leng,size,rsize;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETDIRSTATS - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_get_dir_stats(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,&inodes,&dirs,&files,&chunks,&leng,&size,&rsize);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETDIRSTATS,(status!=STATUS_OK)?5:44);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,inodes);
		put32bit(&ptr,dirs);
		put32bit(&ptr,files);
		put32bit(&ptr,chunks);
		put64bit(&ptr,leng);
		put64bit(&ptr,size);
		put64bit(&ptr,rsize);
	}
}

void matocuserv_fuse_gettrash(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint32_t dleng;
	if (length!=4) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETTRASH - wrong size (%"PRIu32"/4)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	status = fs_readtrash_size(eptr->sesdata->rootinode,eptr->sesdata->sesflags,&dleng);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETTRASH,(status!=STATUS_OK)?5:(4+dleng));
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		fs_readtrash_data(eptr->sesdata->rootinode,eptr->sesdata->sesflags,ptr);
	}
}

void matocuserv_fuse_getdetachedattr(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint8_t attr[35];
	uint32_t msgid;
	uint8_t dtype;
	uint8_t *ptr;
	uint8_t status;
	if (length<8 || length>9) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETDETACHEDATTR - wrong size (%"PRIu32"/8,9)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	if (length==9) {
		dtype = get8bit(&data);
	} else {
		dtype = DTYPE_UNKNOWN;
	}
	status = fs_getdetachedattr(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,attr,dtype);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETDETACHEDATTR,(status!=STATUS_OK)?5:39);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		memcpy(ptr,attr,35);
	}
}

void matocuserv_fuse_gettrashpath(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint32_t pleng;
	uint8_t *path;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETTRASHPATH - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_gettrashpath(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,&pleng,&path);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETTRASHPATH,(status!=STATUS_OK)?5:8+pleng+1);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr,pleng+1);
		if (pleng>0) {
			memcpy(ptr,path,pleng);
		}
		ptr[pleng]=0;
	}
}

void matocuserv_fuse_settrashpath(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	const uint8_t *path;
	uint32_t pleng;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;
	if (length<12) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SETTRASHPATH - wrong size (%"PRIu32"/>=12)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	pleng = get32bit(&data);
	if (length!=12+pleng) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_SETTRASHPATH - wrong size (%"PRIu32"/%"PRIu32")",length,12+pleng);
		eptr->mode = KILL;
		return;
	}
	path = data;
	data += pleng;
	while (pleng>0 && path[pleng-1]==0) {
		pleng--;
	}
	status = fs_settrashpath(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode,pleng,path);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_SETTRASHPATH,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}

void matocuserv_fuse_undel(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_UNDEL - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_undel(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_UNDEL,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}

void matocuserv_fuse_purge(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t inode;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	if (length!=8) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_PURGE - wrong size (%"PRIu32"/8)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	inode = get32bit(&data);
	status = fs_purge(eptr->sesdata->rootinode,eptr->sesdata->sesflags,inode);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_PURGE,5);
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	put8bit(&ptr,status);
}


void matocuserv_fuse_getreserved(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint32_t dleng;
	if (length!=4) {
		MFSLOG(LOG_NOTICE,"CUTOMA_FUSE_GETRESERVED - wrong size (%"PRIu32"/4)",length);
		eptr->mode = KILL;
		return;
	}
	msgid = get32bit(&data);
	status = fs_readreserved_size(eptr->sesdata->rootinode,eptr->sesdata->sesflags,&dleng);
	ptr = matocuserv_createpacket(eptr,MATOCU_FUSE_GETRESERVED,(status!=STATUS_OK)?5:(4+dleng));
	if (ptr==NULL) {
		MFSLOG(LOG_NOTICE,"can't allocate memory for packet");
		eptr->mode = KILL;
		return;
	}
	put32bit(&ptr,msgid);
	if (status!=STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		fs_readreserved_data(eptr->sesdata->rootinode,eptr->sesdata->sesflags,ptr);
	}
}



void matocu_session_timedout(session *sesdata) {
	filelist *fl,*afl;
	fl=sesdata->openedfiles;
	while (fl) {
		afl = fl;
		fl=fl->next;
		fs_release(afl->inode,sesdata->sessionid);
		free(afl);
	}
	sesdata->openedfiles=NULL;
	if (sesdata->info) {
		free(sesdata->info);
	}
}

void matocu_session_check(void) {
	session **sesdata,*asesdata;
	uint32_t now;

	now = get_current_time();
	sesdata = &(sessionshead);
	while ((asesdata=*sesdata)) {
		if (asesdata->nsocks==0 && ((asesdata->newsession && asesdata->disconnected+NEWSESSION_TIMEOUT<now) || (asesdata->newsession==0 && asesdata->disconnected+OLDSESSION_TIMEOUT<now))) {
			matocu_session_timedout(asesdata);
			*sesdata = asesdata->next;
			free(asesdata);
		} else {
			sesdata = &(asesdata->next);
		}
	}
}

void matocu_session_statsmove(void) {
	session *sesdata;

       /*
         * slave should not store session file, consider such situation, in 59:59 we got
         * session file from master, but in 60:00 we store the session and cover it, 
         * then the master is killed and we switch to master, and some master's
         * session imformation would missed.
         *
         * Dongyang Zhang
         */
       if(isslave()) {
             return;  
       }
    
	for (sesdata = sessionshead ; sesdata ; sesdata=sesdata->next) {
		memcpy(sesdata->lasthouropstats,sesdata->currentopstats,4*16);
		memset(sesdata->currentopstats,0,4*16);
	}
	matocuserv_store_sessions();
}

void matocu_beforedisconnect(serventry *eptr) {
	chunklist *cl,*acl;
// unlock locked chunks
	cl=eptr->chunkdelayedops;
	while (cl) {
		acl = cl;
		cl=cl->next;
		if (acl->type == FUSE_TRUNCATE) {
			fs_end_setlength(acl->chunkid);
		}
		free(acl);
	}
	eptr->chunkdelayedops=NULL;
	if (eptr->sesdata) {
		if (eptr->sesdata->nsocks>0) {
			eptr->sesdata->nsocks--;
		}
		if (eptr->sesdata->nsocks==0) {
			eptr->sesdata->disconnected = get_current_time();
		}
	}
}

void matocuserv_gotpacket(serventry *eptr,uint32_t type,const uint8_t *data,uint32_t length) {
	if (type==ANTOAN_NOP) {
		return;
	}
	if (eptr->registered==0) {	// sesdata is NULL
		switch (type) {
			case CUTOMA_FUSE_REGISTER:
				matocuserv_fuse_register(eptr,data,length);
				break;
			case CUTOMA_CSERV_LIST:
				matocuserv_cserv_list(eptr,data,length);
				break;
			case CUTOMA_SESSION_LIST:
				matocuserv_session_list(eptr,data,length);
				break;
			case CUTOAN_CHART:
				matocuserv_chart(eptr,data,length);
				break;
			case CUTOAN_CHART_DATA:
				matocuserv_chart_data(eptr,data,length);
				break;
			case CUTOMA_INFO:
				matocuserv_info(eptr,data,length);
				break;
			case CUTOMA_FSTEST_INFO:
				matocuserv_fstest_info(eptr,data,length);
				break;
			case CUTOMA_CHUNKSTEST_INFO:
				matocuserv_chunkstest_info(eptr,data,length);
				break;
			case CUTOMA_CHUNKS_MATRIX:
				matocuserv_chunks_matrix(eptr,data,length);
				break;
			case CUTOMA_QUOTA_INFO:
				matocuserv_quota_info(eptr,data,length);
				break;
			case CUTOMA_EXPORTS_INFO:
				matocuserv_exports_info(eptr,data,length);
				break;
			case CUTOMA_MLOG_LIST:
				matocuserv_mlog_list(eptr,data,length);
				break;
			default:
				MFSLOG(LOG_NOTICE,"matocu: got unknown message from unregistered (type:%"PRIu32")",type);
				eptr->mode=KILL;
		}
	} else if (eptr->registered<100) {
		if (eptr->sesdata==NULL) {
			MFSLOG(LOG_ERR,"registered connection without sesdata !!!");
			eptr->mode=KILL;
			return;
		}
		switch (type) {
			case CUTOMA_FUSE_RESERVED_INODES:
				matocuserv_fuse_reserved_inodes(eptr,data,length);
				break;
			case CUTOMA_FUSE_STATFS:
				matocuserv_fuse_statfs(eptr,data,length);
				break;
			case CUTOMA_FUSE_ACCESS:
				matocuserv_fuse_access(eptr,data,length);
				break;
			case CUTOMA_FUSE_LOOKUP:
				matocuserv_fuse_lookup(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETATTR:
				matocuserv_fuse_getattr(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETATTR:
				matocuserv_fuse_setattr(eptr,data,length);
				break;
			case CUTOMA_FUSE_READLINK:
				matocuserv_fuse_readlink(eptr,data,length);
				break;
			case CUTOMA_FUSE_SYMLINK:
				matocuserv_fuse_symlink(eptr,data,length);
				break;
			case CUTOMA_FUSE_MKNOD:
				matocuserv_fuse_mknod(eptr,data,length);
				break;
			case CUTOMA_FUSE_MKDIR:
				matocuserv_fuse_mkdir(eptr,data,length);
				break;
			case CUTOMA_FUSE_UNLINK:
				matocuserv_fuse_unlink(eptr,data,length);
				break;
			case CUTOMA_FUSE_RMDIR:
				matocuserv_fuse_rmdir(eptr,data,length);
				break;
			case CUTOMA_FUSE_RENAME:
				matocuserv_fuse_rename(eptr,data,length);
				break;
			case CUTOMA_FUSE_LINK:
				matocuserv_fuse_link(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETDIR:
				matocuserv_fuse_getdir(eptr,data,length);
				break;
			case CUTOMA_FUSE_OPEN:
				matocuserv_fuse_open(eptr,data,length);
				break;
			case CUTOMA_FUSE_READ_CHUNK:
				matocuserv_fuse_read_chunk(eptr,data,length);
				break;
			case CUTOMA_FUSE_WRITE_CHUNK:
				matocuserv_fuse_write_chunk(eptr,data,length);
				break;
			case CUTOMA_FUSE_WRITE_CHUNK_END:
				matocuserv_fuse_write_chunk_end(eptr,data,length);
				break;
// fuse - meta
			case CUTOMA_FUSE_GETTRASH:
				matocuserv_fuse_gettrash(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETDETACHEDATTR:
				matocuserv_fuse_getdetachedattr(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETTRASHPATH:
				matocuserv_fuse_gettrashpath(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETTRASHPATH:
				matocuserv_fuse_settrashpath(eptr,data,length);
				break;
			case CUTOMA_FUSE_UNDEL:
				matocuserv_fuse_undel(eptr,data,length);
				break;
			case CUTOMA_FUSE_PURGE:
				matocuserv_fuse_purge(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETRESERVED:
				matocuserv_fuse_getreserved(eptr,data,length);
				break;

// extra (external tools - still here for compatibility with old tools)
			case CUTOMA_FUSE_CHECK:
				matocuserv_fuse_check(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETTRASHTIME:
				matocuserv_fuse_gettrashtime(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETTRASHTIME:
				matocuserv_fuse_settrashtime(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETGOAL:
				matocuserv_fuse_getgoal(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETGOAL:
				matocuserv_fuse_setgoal(eptr,data,length);
				break;
			case CUTOMA_FUSE_APPEND:
				matocuserv_fuse_append(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETDIRSTATS:
				matocuserv_fuse_getdirstats_old(eptr,data,length);
				break;
			case CUTOMA_FUSE_TRUNCATE:
				matocuserv_fuse_truncate(eptr,data,length);
				break;
			default:
				MFSLOG(LOG_NOTICE,"matocu: got unknown message from mfsmount (type:%"PRIu32")",type);
				eptr->mode=KILL;
		}
	} else {
		if (eptr->sesdata==NULL) {
			MFSLOG(LOG_ERR,"registered connection (tools) without sesdata !!!");
			eptr->mode=KILL;
			return;
		}
		switch (type) {
// extra (external tools)
			case CUTOMA_FUSE_READ_CHUNK:	// used in mfsfileinfo
				matocuserv_fuse_read_chunk(eptr,data,length);
				break;
			case CUTOMA_FUSE_CHECK:
				matocuserv_fuse_check(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETTRASHTIME:
				matocuserv_fuse_gettrashtime(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETTRASHTIME:
				matocuserv_fuse_settrashtime(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETGOAL:
				matocuserv_fuse_getgoal(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETGOAL:
				matocuserv_fuse_setgoal(eptr,data,length);
				break;
			case CUTOMA_FUSE_APPEND:
				matocuserv_fuse_append(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETDIRSTATS:
				matocuserv_fuse_getdirstats(eptr,data,length);
				break;
			case CUTOMA_FUSE_TRUNCATE:
				matocuserv_fuse_truncate(eptr,data,length);
				break;
			case CUTOMA_FUSE_REPAIR:
				matocuserv_fuse_repair(eptr,data,length);
				break;
			case CUTOMA_FUSE_SNAPSHOT:
				matocuserv_fuse_snapshot(eptr,data,length);
				break;
			case CUTOMA_FUSE_GETEATTR:
				matocuserv_fuse_geteattr(eptr,data,length);
				break;
			case CUTOMA_FUSE_SETEATTR:
				matocuserv_fuse_seteattr(eptr,data,length);
				break;
/* do not use in version before 1.7.x */
			case CUTOMA_FUSE_QUOTACONTROL:
				matocuserv_fuse_quotacontrol(eptr,data,length);
				break;
/* ------ */
			default:
				MFSLOG(LOG_NOTICE,"matocu: got unknown message from mfstools (type:%"PRIu32")",type);
				eptr->mode=KILL;
		}
	}
}

void matocuserv_term(void) {
	serventry *eptr,*eaptr;
	packetstruct *pptr,*paptr;
	MFSLOG(LOG_INFO,"matocu: closing %s:%s",ListenHost,ListenPort);
	tcpclose(lsock);

	eptr = matocuservhead;
	while (eptr) {
		if (eptr->inputpacket.packet) {
			free(eptr->inputpacket.packet);
		}
		pptr = eptr->outputhead;
		while (pptr) {
			if (pptr->packet) {
				free(pptr->packet);
			}
			paptr = pptr;
			pptr = pptr->next;
			free(paptr);
		}
		eaptr = eptr;
		eptr = eptr->next;
		free(eaptr);
	}
	matocuservhead=NULL;
}

void matocuserv_read(serventry *eptr) {
	int32_t i;
	uint32_t type,size;
	const uint8_t *ptr;
    
	for (;;) {
		i=read(eptr->sock,eptr->inputpacket.startptr,eptr->inputpacket.bytesleft);
		if (i==0) {
			// syslog(LOG_INFO,"matocu: connection lost");
			eptr->mode = KILL;
			return;
		}
		if (i<0) {
			if (errno!=EAGAIN) {
#ifdef ECONNRESET
				if (errno!=ECONNRESET || eptr->registered<100) {
#endif
					MFSLOG(LOG_INFO,"matocu: read error: %m");
#ifdef ECONNRESET
				}
#endif
				eptr->mode = KILL;
			}
			return;
		}
		eptr->inputpacket.startptr+=i;
		eptr->inputpacket.bytesleft-=i;

		if (eptr->inputpacket.bytesleft>0) {
			return;
		}

		if (eptr->mode==HEADER) {
			ptr = eptr->hdrbuff+4;
			size = get32bit(&ptr);

			if (size>0) {
				if (size>MaxPacketSize) {
					MFSLOG(LOG_WARNING,"matocu: packet too long (%"PRIu32"/%u)",size,MaxPacketSize);
					eptr->mode = KILL;
					return;
				}
				eptr->inputpacket.packet = malloc(size);
				if (eptr->inputpacket.packet==NULL) {
					MFSLOG(LOG_WARNING,"matocu: out of memory");
					eptr->mode = KILL;
					return;
				}
				eptr->inputpacket.bytesleft = size;
				eptr->inputpacket.startptr = eptr->inputpacket.packet;
				eptr->mode = DATA;
				continue;
			}
			eptr->mode = DATA;
		}

		if (eptr->mode==DATA) {
			ptr = eptr->hdrbuff;
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode=HEADER;
			eptr->inputpacket.bytesleft = 8;
			eptr->inputpacket.startptr = eptr->hdrbuff;

			matocuserv_gotpacket(eptr,type,eptr->inputpacket.packet,size);

			if (eptr->inputpacket.packet) {
				free(eptr->inputpacket.packet);
			}
			eptr->inputpacket.packet=NULL;
		}
	}
}

void matocuserv_write(serventry *eptr) {
	packetstruct *pack;
	int32_t i;
	for (;;) {
		pack = eptr->outputhead;
		if (pack==NULL) {
			return;
		}
		i=write(eptr->sock,pack->startptr,pack->bytesleft);
		if (i<0) {
			if (errno!=EAGAIN) {
				MFSLOG(LOG_INFO,"matocu: write error: %m");
				eptr->mode = KILL;
			}
			return;
		}
		pack->startptr+=i;
		pack->bytesleft-=i;
		if (pack->bytesleft>0) {
			return;
		}
		free(pack->packet);
		eptr->outputhead = pack->next;
		if (eptr->outputhead==NULL) {
			eptr->outputtail = &(eptr->outputhead);
		}
		free(pack);
	}
}

void matocuserv_wantexit(void) {
	exiting=1;
}

int matocuserv_canexit(void) {
	serventry *eptr;
	for (eptr=matocuservhead ; eptr ; eptr=eptr->next) {
		if (eptr->outputhead!=NULL) {
			return 0;
		}
		if (eptr->chunkdelayedops!=NULL) {
			return 0;
		}
	}
	return 1;
}

void matocuserv_desc(int epoll_fd) {
	uint32_t now=get_current_time();
	serventry *eptr,**kptr,**wptr;
        packetstruct *pptr,*paptr;
	int ret;
	struct epoll_event ev = {0,{0}};

	if ((first_add_listen_sock==0) && (exiting==0)) {
		eptr = (serventry *)malloc(sizeof(serventry));
                eptr->next = matocuservhead;
                matocuservhead = eptr;
                eptr->sock = lsock;
                eptr->registered = 0;
                eptr->version = 0;
                eptr->mode = HEADER;
                eptr->lastread =  eptr->lastwrite = get_current_time();
                eptr->inputpacket.next = NULL;
                eptr->inputpacket.bytesleft = 8;
                eptr->inputpacket.startptr = eptr->hdrbuff;
                eptr->inputpacket.packet = NULL;
                eptr->outputhead = NULL;
                eptr->outputtail = &(eptr->outputhead);
		eptr->chunkdelayedops = NULL;
                eptr->sesdata = NULL;
                memset(eptr->passwordrnd,0,32);
		
		eptr->listen_sock = 1;
                eptr->connection = 0;

		ev.data.ptr = eptr;
		ev.events = EPOLLIN;
		ret = epoll_ctl(epoll_fd,EPOLL_CTL_ADD,lsock,&ev);
		if(ret!=0) {
			MFSLOG(LOG_NOTICE,"epoll_ctl error");
		}
 
//		syslog(LOG_NOTICE,"listen_socket:connection:%d,lastread:%d,lastwrite:%d,eptr_sock:%d,listen_sock:%d,registered:%d,version:%d",eptr->connection,eptr->lastread,eptr->lastwrite,eptr->sock,eptr->listen_sock,eptr->registered,eptr->version);
		first_add_listen_sock = 1;
		lsockpdescpos = 1;
	} else if(exiting==1) {
		lsockpdescpos = -1;
	}
	kptr = &matocuservhead;
	wptr = &matocuservhead;
	while((eptr=*kptr)) {
		if (eptr->listen_sock == 0 && eptr->mode != KILL) {
			ev.data.ptr = eptr;
	        	if (exiting==0 && eptr->mode != KILL) {
				ev.events = EPOLLIN;
			}		
			if (eptr->outputhead!=NULL && eptr->mode != KILL) {
				ev.events = EPOLLIN|EPOLLOUT;
			}		
			ret = epoll_ctl(epoll_fd,EPOLL_CTL_MOD,eptr->sock,&ev);
			if(ret!=0) {
                		MFSLOG(LOG_NOTICE,"epoll_ctl error");
                	}
		}

		if (eptr->listen_sock == 1) {
			eptr->lastread = eptr->lastwrite = get_current_time();
		}
		if (eptr->lastread+10<now && exiting==0) {
                        eptr->mode = KILL;
                }
                if (eptr->lastwrite+2<now && eptr->registered<100 && eptr->outputhead==NULL) {
                        uint8_t *ptr = matocuserv_createpacket(eptr,ANTOAN_NOP,4);      // 4 byte length because of 'msgid'
                        *((uint32_t*)ptr) = 0;
                }
		if (eptr->mode == KILL) {
			ev.data.ptr = eptr;
			matocu_beforedisconnect(eptr);
			epoll_ctl(epoll_fd,EPOLL_CTL_DEL,eptr->sock,&ev);			
			tcpclose(eptr->sock);
			if (eptr->inputpacket.packet) {
				free(eptr->inputpacket.packet);
			}
			pptr = eptr->outputhead;
			while (pptr) {
				if (pptr->packet) {
					free(pptr->packet);
				}
				paptr = pptr;
				pptr = pptr->next;
				free(paptr);
			}
			if(eptr == matocuservhead) {
                                matocuservhead = eptr->next;
                                wptr = &matocuservhead;
                        }
                        else {
                                (*wptr)->next = eptr->next;
                        }
                        *kptr = eptr->next;
			free(eptr);
		} else {
		        wptr = &eptr;
                        kptr = &(eptr->next);
		}
	}
}


void matocuserv_serve(int epoll_fd,int count,struct epoll_event *pdesc) {
	uint32_t now=get_current_time();
	serventry *eptr,*weptr;
	int ns;
	struct epoll_event ev = {0,{0}};
	
	weptr = (serventry*)pdesc[count].data.ptr;
	if((weptr->listen_sock == 1) && (lsockpdescpos >= 0) && (pdesc[count].events & EPOLLIN) && (meta_ready == 0) ) {
		ns = tcpaccept(lsock);
		if (ns < 0) {
			MFSLOG(LOG_INFO,"matocu: accept error: %m");
		} else {
			tcpnonblock(ns);
	       		tcpnodelay(ns);
		        eptr = (serventry *)malloc(sizeof(serventry));
			eptr->next = matocuservhead;
	                matocuservhead = eptr;
	                eptr->sock = ns;
	                tcpgetpeer(ns,&(eptr->peerip),NULL);
	                eptr->registered = 0;
	                eptr->version = 0;
	                eptr->mode = HEADER;
	                eptr->lastread = eptr->lastwrite = get_current_time();
	                eptr->inputpacket.next = NULL;
	                eptr->inputpacket.bytesleft = 8;
	                eptr->inputpacket.startptr = eptr->hdrbuff;
	                eptr->inputpacket.packet = NULL;
	                eptr->outputhead = NULL;
	                eptr->outputtail = &(eptr->outputhead);

	                eptr->chunkdelayedops = NULL;
	                eptr->sesdata = NULL;
	                memset(eptr->passwordrnd,0,32);
					               
                        eptr->listen_sock = 0;
                        eptr->connection = 0;
 
			ev.data.ptr = eptr;
             		ev.events = EPOLLIN;
                	epoll_ctl(epoll_fd,EPOLL_CTL_ADD,ns,&ev);
              	}
	} 
        if((weptr->listen_sock == 0) && (meta_ready == 0) ) {
		if (pdesc[count].events & (EPOLLERR|EPOLLHUP)) {
       			weptr->mode = KILL;
                }
                if ((pdesc[count].events & EPOLLIN) && weptr->mode!=KILL) {
                        matocuserv_read(weptr);
                        weptr->lastread = now;						
                }
                if ((pdesc[count].events & EPOLLOUT) && weptr->mode!=KILL && weptr->outputhead!=NULL) {
                        matocuserv_write(weptr);
                        weptr->lastwrite = now;						
                }
	}               
}

int matocuserv_sessionsinit() {
	fprintf(msgfd,"loading sessions ...\n");
	fflush(msgfd);
	sessionshead = NULL;
	if (matocuserv_load_sessions()<0) {
        fprintf(msgfd,"error\n");
        fprintf(msgfd,"due to missing sessions you have to restart all active mounts !!!\n");
    } else {
        fprintf(msgfd,"ok\n");
        fprintf(msgfd,"sessions file has been loaded\n");
	}
	return 0;
}

int matocuserv_networkinit() {
	ListenHost = cfg_getstr("MATOCU_LISTEN_HOST","*");
	ListenPort = cfg_getstr("MATOCU_LISTEN_PORT","9421");
	RejectOld = cfg_getuint32("REJECT_OLD_CLIENTS",0);

	/* as master, the meta data is ok */
	if(ismaster()) {
		meta_ready = 0;
	}

	exiting = 0;
	first_add_listen_sock = 0;
	lsock = tcpsocket();
	if (lsock<0) {
		MFSLOG(LOG_ERR,"matocu: socket error: %m");
		fprintf(msgfd,"main master server module: can't create socket\n");
		return -1;
	}
	tcpnonblock(lsock);
	tcpnodelay(lsock);
	tcpreuseaddr(lsock);
	if (tcpsetacceptfilter(lsock)<0) {
		MFSLOG(LOG_NOTICE,"matocu: can't set accept filter: %m");
	}
	if (tcpstrlisten(lsock,ListenHost,ListenPort,1024)<0) {
		MFSLOG(LOG_ERR,"matocu: listen error: %m");
		fprintf(msgfd,"main master server module: can't listen on socket\n");
		return -1;
	}
	MFSLOG(LOG_NOTICE,"matocu: listen on %s:%s",ListenHost,ListenPort);
	fprintf(msgfd,"main master server module: listen on %s:%s\n",ListenHost,ListenPort);

	matocuservhead = NULL;

	main_timeregister(TIMEMODE_RUNONCE,10,0,matocu_session_check);
	main_timeregister(TIMEMODE_RUNONCE,3600,0,matocu_session_statsmove);
	main_destructregister(matocuserv_term);
	main_epollregister(matocuserv_desc,matocuserv_serve);
	main_wantexitregister(matocuserv_wantexit);
	main_canexitregister(matocuserv_canexit);
	return 0;
}
