/*                            xmove
 *                            -----
 *             A Pseudoserver For Client Mobility
 *
 *   Copyright (c) 1994         Ethan Solomita
 *
 *   The X Consortium, and any party obtaining a copy of these files from
 *   the X Consortium, directly or indirectly, is granted, free of charge, a
 *   full and unrestricted irrevocable, world-wide, paid up, royalty-free,
 *   nonexclusive right and license to deal in this software and
 *   documentation files (the "Software"), including without limitation the
 *   rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons who receive
 *   copies from any such party to do so.  This license includes without
 *   limitation a license to do the foregoing actions under any patents of
 *   the party supplying this software to the X Consortium.
 */
/* Portions of this file contain source code based upon Xlib, whose
   source is copyrighted as below: */
/* $XConsortium: XGetProp.c,v 11.18 92/01/27 20:14:23 gildea Exp $ */
/* Copyright    Massachusetts Institute of Technology    1986	*/

#include <stdio.h>

#include <X11/Xatom.h>
#include <errno.h>             /* for EINTR, EADDRINUSE, ... */
#include <malloc.h>
#include <sys/ioctl.h>
#ifdef SYSV
#include <sys/fcntl.h>
#endif

#include "xmove.h"
#include "xmond.h"


/* The following code allows xmove to insert additional
 * requests into the client's server connection, without 
 * worrying about problems due to sequence numbers becoming
 * out of sync. The method requires the following of its
 * users: it must insert *at least* one request which involves
 * a reply, and if there are any requests following the last
 * request which involves a reply, they *must not* result in
 * the server sending an error or event.
 *
 * So long as the two rules above are followed, sequence
 * numbers will never go out of sync. If you want to insert
 * requests, but none involve a reply, use a spurious.
 * GetKeyboardFocus request at the end.
 */

/* insert_seqno is the sequence number of the last
 * request sent by the client itself.
 */

static signed long insert_seqno;
static int insert_count = 0;	/* number of calls to init - calls to done */

/* We use insert_seqno to store the sequence number as of the
 * first inserted request.
 */

void
InsertRequestsInit()
{
     if (insert_count++ != 0)
	  return;
     
     while (server->fdd->outBuffer.num_Saved)
	  SendXmoveBuffer(server->fdd->fd, &server->fdd->outBuffer);
     
     /* processing_server is True if the stack trace leads back to
      * DataFromServer, False if to DataFromClient.
      */
     
     if (!processing_server)
	  client->SequenceNumber--;	/* necessary: DecodeRequest bumped it */
     
     insert_seqno = (long)client->SequenceNumber;
}

void
InsertRequestsDone()
{
     if (--insert_count != 0)
	  return;
     
     client->SequenceMapping -= client->SequenceNumber - insert_seqno;
     
     if (!processing_server)
	  client->SequenceNumber++;
}

Bool
SendBuffer(int fd, unsigned char *buf, int BytesToWrite)
{
    int			num_written;
#ifndef SVR4
    int                 NBIO = 0;         /* used in ioctl */
#endif

    if (BytesToWrite > 0)
    {
/* does the following really help? probably not -- extra overhead
   trapping to kernel
#ifdef SVR4
	 fcntl(fd, F_SETFL, ~O_NONBLOCK & fcntl(fd, F_GETFL, NULL));
#else
	 ioctl(fd, FIONBIO, (char *)&NBIO);
#endif
*/
	 do
	 {
	      num_written = write (fd, (char *)buf, BytesToWrite);
	      if (num_written > 0)
	      {
		   BytesToWrite -= num_written;
		   buf += num_written;
	      }
	 }
	 while (BytesToWrite > 0 && (num_written > 0 || errno == EWOULDBLOCK ||
				     errno == EINTR || errno == EAGAIN));
	 
/*#ifdef SVR4
	 fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL, NULL));
#else
	 NBIO = 1;
	 ioctl(fd, FIONBIO, (char *)&NBIO);
#endif*/

	 if (BytesToWrite > 0)
	 {
	      /* could not send request to server.  What should we do? */
	      Dprintf(("send buffer failed\n"));
	      return False;
	 }
    }
    return True;
}


/* do *not* use the following function directly if you are
 * inserting requests into the connection between client and
 * server, unless we are currently moving the client. Use one
 * of the Receive functions below.
 */

Bool
ReceiveBuffer(int fd, unsigned char *buf, int BytesToRead)
{
    int			num_read;
#ifndef SVR4
    int                 NBIO = 0;         /* used in ioctl */
#endif

    if (BytesToRead > 0)
    {
/*#ifdef SVR4
	 fcntl(fd, F_SETFL, ~O_NONBLOCK & fcntl(fd, F_GETFL, NULL));
#else
	 ioctl(fd, FIONBIO, (char *)&NBIO);
#endif*/
	/* loop until we get all the bytes promised us */
	do
	{
	    num_read = read (fd, (char *)buf, BytesToRead);
	    if (num_read > 0)
	    {
		BytesToRead -= num_read;
		buf += num_read;
	    }
	}
	while (BytesToRead > 0);
/*#ifdef SVR4
	 fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL, NULL));
#else
	 NBIO = 1;
	 ioctl(fd, FIONBIO, (char *)&NBIO);
#endif*/
    }
    return True;
}

/*
 * ReceiveReply will continuously read from fd until it finds an
 * message with SequenceNumber. If it is a reply, it will return True,
 * otherwise it'll return False.
 *
 * If InsertRequestsInit was called previously, anything it retrieves
 * before the reply, which the client is expecting, will be stored in
 * the server's input buffer with the sequence number appropriately modified.
 */

Bool
ReceiveReply(int fd, unsigned char *buf, int BytesToRead, unsigned short SequenceNumber)
{
     int retval, remaining;
     Buffer *inbuffer;
     
     if (!MoveInProgress && insert_count == 0)
	     panic("xmove assert: ReceiveReply when not moving and not inserting\n");

     if (MoveInProgress && new_server && fd == new_fd)
	     inbuffer = new_server->fdd ? &new_server->fdd->inBuffer : NULL;
     else if (!MoveInProgress)
	     inbuffer = server->fdd ? &server->fdd->inBuffer : NULL;
     else
	     inbuffer = NULL;

     if (insert_count != 0 && inbuffer && inbuffer->incomplete_data) {
	  int amount_read = inbuffer->num_Saved - (inbuffer->incomplete_data - inbuffer->data);

	  if (amount_read < 32) {
	       ReceiveBuffer(fd, buf, 32 - amount_read);
	       Eprintf(("ETHAN: ReceiveReply 1 saving %d bytes\n", 32-amount_read));
	       SaveBytes(inbuffer, &buf[0], 32 - amount_read);
	  }

	  if (amount_read < 8 && IByte(inbuffer->incomplete_data) != 11)
	  {
	       ISetShort(inbuffer->incomplete_data + 2,
			 (u_short)(IShort(inbuffer->incomplete_data + 2) + client->SequenceMapping));
	  }

	  if (IByte(inbuffer->incomplete_data) == 1) {
	       int amount_to_read = ILong(inbuffer->incomplete_data + 4) << 2;
	       
	       while (amount_to_read > 32) {
		    ReceiveBuffer(fd, buf, 32);
		    Eprintf(("ETHAN: ReceiveReply 2 saving %d bytes\n", 32));
		    SaveBytes(inbuffer, &buf[0], 32);
		    amount_to_read -= 32;
	       }

	       ReceiveBuffer(fd, buf, amount_to_read);
	       Eprintf(("ETHAN: ReceiveReply 3 saving %d bytes\n", amount_to_read));
	       SaveBytes(inbuffer, &buf[0], amount_to_read);
	  }

	  Eprintf(("ETHAN: ReceiveReply clearing incomplete_data\n"));
	  inbuffer->incomplete_data = NULL;
     }

     while (1) {
	  u_short cur_seqno;
	  u_char  cur_code;
	  
	  retval = ReceiveBuffer(fd, buf, 32);
	  if (((u_char)(IByte(&buf[0]) & 0x7F)) > (u_char)MAX_EVENT) {
	       return False;
	  }
	       
	  cur_seqno = IShort(&buf[2]);
	  cur_code  = IByte(&buf[0]) & 0x7F;

	  if (cur_seqno == SequenceNumber) {
	       if (cur_code == 1) {       /* reply found */
		    retval = ReceiveBuffer(fd, buf+32, BytesToRead-32);
		    return True;
	       } else if (cur_code == 0)  /* error found */
		    return False;
	  }
	      
	  if (inbuffer == NULL)
		  continue;
	  
	  if (insert_count != 0) {
	       if ((int)cur_seqno <= insert_seqno) {
		    if (cur_code != 11)
			 ISetShort(&buf[2], (u_short)(cur_seqno + client->SequenceMapping));
		    Eprintf(("ETHAN: ReceiveReply 4 saving %d bytes\n", 32));
		    SaveBytes(inbuffer, &buf[0], 32);
		    
		    if (cur_code == 1) { /* may be longer than 32 bytes */
			 remaining = ILong(&buf[4]) << 2;
			 
			 while (remaining >= 32) {
			      retval = ReceiveBuffer(fd, buf, 32);
			      Eprintf(("ETHAN: ReceiveReply 5 saving %d bytes\n", 32));
			      SaveBytes(inbuffer, &buf[0], 32);
			      remaining -= 32;
			 }
			 
			 if (remaining) {
			      retval = ReceiveBuffer(fd, buf, remaining);
			      Eprintf(("ETHAN: ReceiveReply 6 saving %d bytes\n", remaining));
			      SaveBytes(inbuffer, &buf[0], remaining);
			 }
		    }
	       } else if (cur_code > 1) {
		    /* set seqno to be that of the last request the client has sent */
		    
		    if (cur_code != 11)
			 ISetShort(&buf[2], (u_short)(insert_seqno + client->SequenceMapping));
		    Eprintf(("ETHAN: ReceiveReply 7 saving %d bytes\n", 32));
		    SaveBytes(inbuffer, &buf[0], 32);
	       }
	  } else if (MoveInProgress && cur_code > 1) {
		  if (cur_code != 11)
			  ISetShort(&buf[2], client->LastMoveSeqNo);
		  Eprintf(("ETHAN: ReceiveReply 8 saving %d bytes, cur_code %d\n", 32, cur_code));
		  SaveBytes(inbuffer, &buf[0], 32);
	  }
     }
}


/* the following function is currently only used during a move,
   so it hasn't been adapted to save unused messages as has ReceiveReply */

Bool
ReceiveReplyWError(int fd, unsigned char *buf, int BytesToRead, unsigned short SequenceNumber, Bool *error)
{
	Buffer *inbuffer;
     
	if (new_server && fd == new_fd)
		inbuffer = new_server->fdd ? &new_server->fdd->inBuffer : NULL;
	else
		inbuffer = NULL;

	*error = False;
	while (1) {
		int code, seqno;
		
		ReceiveBuffer(fd, buf, 32);
		
		code = IByte(&buf[0]);
		seqno = IShort(&buf[2]);
		
		if (code == 0) {
			*error = True;
			return True;
			
		} else if (seqno == SequenceNumber && code == 1) {
			if (code == 0)  /* error found */
				*error = True;
			else
				ReceiveBuffer(fd, buf+32, BytesToRead-32);
			return True;
			
		} else if (inbuffer && code > 1) {
			if (code != 11)
				ISetShort(&buf[2], client->LastMoveSeqNo);
			Eprintf(("ETHAN: ReceiveReplyWError saving 32 bytes, code %d\n", code));
			SaveBytes(inbuffer, &buf[0], 32);
		}
	}
}

Font
XMOVELoadFont(fd, seqno, id, name)
int fd;
unsigned short *seqno;
Font id;
char *name;
{
     xOpenFontReq OpenFontRequest;
     int namelen = strlen(name);
     int totlen = ROUNDUP4(namelen);
     char *LongName = calloc(totlen, 1);

     Dprintf(("in XMOVELoadFont n = %d, name = %s\n",namelen,name));

     OpenFontRequest.reqType = X_OpenFont;
     ISetShort((unsigned char *)&OpenFontRequest.length, 3 + (totlen >> 2));
     ISetLong((unsigned char *)&OpenFontRequest.fid, id);
     ISetShort((unsigned char *)&OpenFontRequest.nbytes, namelen);

     LongName = strncpy(LongName, name, totlen);
     
     SendBuffer(fd, (unsigned char *)&OpenFontRequest, sz_xOpenFontReq);
     SendBuffer(fd, (unsigned char *)LongName, totlen);

     ++(*seqno);

     free(LongName);
     return id;
}


Atom
XMOVEInternAtom(fd, seqno, name, onlyifexists)
int fd;
unsigned short *seqno;
char *name;
Bool onlyifexists;
{
     xInternAtomReq InternAtomRequest;
     xInternAtomReply InternAtomReply;
     
     int namelen = strlen(name);
     int totlen = ROUNDUP4(namelen);
     char *LongName = calloc(totlen, 1);

     Dprintf(("in XMOVEInternAtom n = %d, name = %s\n",namelen,name));

     InternAtomRequest.reqType      = X_InternAtom;
     InternAtomRequest.onlyIfExists = onlyifexists;
     ISetShort((unsigned char *)&InternAtomRequest.length, 2 + (totlen>>2));
     ISetShort((unsigned char *)&InternAtomRequest.nbytes, namelen);

     LongName = strncpy(LongName, name, totlen);
     
     SendBuffer(fd, (unsigned char *)&InternAtomRequest, sz_xInternAtomReq);
     SendBuffer(fd, (unsigned char *)LongName, totlen);

     free(LongName);
     
     if (ReceiveReply(fd, (unsigned char *)&InternAtomReply,
		      sz_xInternAtomReply, ++(*seqno))) {
	  Dprintf(("InternAtom reply: Atom=%d\n",ILong((unsigned char *)&InternAtomReply.atom)));
	  return ILong((unsigned char *)&InternAtomReply.atom);
     } else {
	  Dprintf(("Read from server failed\n"));
	  return 0;
     }
}


Cursor XMOVECreateGlyphCursor(fd, seqno, id, source_font, mask_font,
			      source_char, mask_char, foreground, background)
int fd;
unsigned short *seqno;
Cursor id;
Font source_font, mask_font;
unsigned int source_char, mask_char;
XColor *foreground, *background;
{
     xCreateGlyphCursorReq CreateGlyphRequest;

     Dprintf(("in XMOVECreateGlyphCursor\n"));

     CreateGlyphRequest.reqType    = X_CreateGlyphCursor;
     ISetShort((unsigned char *)&CreateGlyphRequest.length, 8);
     ISetLong((unsigned char *)&CreateGlyphRequest.cid, id);
     ISetLong((unsigned char *)&CreateGlyphRequest.source, source_font);
     ISetLong((unsigned char *)&CreateGlyphRequest.mask, mask_font);
     ISetShort((unsigned char *)&CreateGlyphRequest.sourceChar, source_char);
     ISetShort((unsigned char *)&CreateGlyphRequest.maskChar, mask_char);
     ISetShort((unsigned char *)&CreateGlyphRequest.foreRed, foreground->red);
     ISetShort((unsigned char *)&CreateGlyphRequest.foreGreen, foreground->green);
     ISetShort((unsigned char *)&CreateGlyphRequest.foreBlue, foreground->blue);
     ISetShort((unsigned char *)&CreateGlyphRequest.backRed, background->red);
     ISetShort((unsigned char *)&CreateGlyphRequest.backGreen, background->green);
     ISetShort((unsigned char *)&CreateGlyphRequest.backBlue, background->blue);

     SendBuffer(fd, (unsigned char *)&CreateGlyphRequest, sz_xCreateGlyphCursorReq);
     ++(*seqno);

     return id;
}

Pixmap
XMOVECreatePixmap(fd, seqno, id, d, width, height, depth)
int fd;
unsigned short *seqno;
Pixmap id;
Drawable d;
unsigned int width, height, depth;
{
     xCreatePixmapReq CreatePixmapRequest;

     Dprintf(("in XMOVECreatePixmap\n"));

     CreatePixmapRequest.reqType  = X_CreatePixmap;
     CreatePixmapRequest.depth    = depth;
     ISetShort((unsigned char *)&CreatePixmapRequest.length, 4);
     ISetLong((unsigned char *)&CreatePixmapRequest.pid, id);
     ISetLong((unsigned char *)&CreatePixmapRequest.drawable, d);
     ISetShort((unsigned char *)&CreatePixmapRequest.width, width);
     ISetShort((unsigned char *)&CreatePixmapRequest.height, height);

     SendBuffer(fd, (unsigned char *)&CreatePixmapRequest, sz_xCreatePixmapReq);
     ++(*seqno);

     return id;
}

void
XMOVESetSelectionOwner(fd, seqno, selection, owner, time)
int fd;
unsigned short *seqno;
Atom selection;
Window owner;
Time time;
{
     xSetSelectionOwnerReq SetSelectionOwnerRequest;

     Dprintf(("in XMOVECreateSetSelectionOwner\n"));

     SetSelectionOwnerRequest.reqType   = X_SetSelectionOwner;
     ISetShort((unsigned char *)&SetSelectionOwnerRequest.length, 4);
     ISetLong((unsigned char *)&SetSelectionOwnerRequest.window, owner);
     ISetLong((unsigned char *)&SetSelectionOwnerRequest.selection, selection);
     ISetLong((unsigned char *)&SetSelectionOwnerRequest.time, time);

     SendBuffer(fd,(unsigned char *)&SetSelectionOwnerRequest,sz_xSetSelectionOwnerReq);
     ++(*seqno);
}


Window
XMOVEGetSelectionOwner(fd, seqno, selection)
int fd;
unsigned short *seqno;
Atom selection;
{
     xResourceReq            GetSelectionOwnerRequest;
     xGetSelectionOwnerReply GetSelectionOwnerReply;

     Dprintf(("in XMOVECreateGetSelectionOwner\n"));

     GetSelectionOwnerRequest.reqType = X_GetSelectionOwner;
     ISetShort((unsigned char *)&GetSelectionOwnerRequest.length, 2);
     ISetLong((unsigned char *)&GetSelectionOwnerRequest.id, selection);

     SendBuffer(fd, (unsigned char *)&GetSelectionOwnerRequest, sz_xResourceReq);
     
     ReceiveReply(fd, (unsigned char *)&GetSelectionOwnerReply,
		  sz_xGetSelectionOwnerReply, ++(*seqno));

     return ILong((unsigned char *)&GetSelectionOwnerReply.owner);
}


GContext
XMOVECreateGC(fd, seqno, id, d, valuemask, values)
int fd;
unsigned short *seqno;
GContext id;
Drawable d;
unsigned long valuemask;
XGCValues *values;
{
     xCreateGCReq CreateGCRequest;
     int numvalues = 0, offset = 0;

     /* to hold the compressed valuelist which can be up to 92 bytes long */
     unsigned char temp_buf[92];

     Dprintf(("in XMOVECreateGC\n"));

     if (valuemask & GCFunction)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->function);
	 offset += 4;
       }

     if (valuemask & GCPlaneMask)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->plane_mask);
	 offset += 4;
       }

     if (valuemask & GCForeground)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->foreground);
	 offset += 4;
       }

     if (valuemask & GCBackground)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->background);
	 offset += 4;
       }

     if (valuemask & GCLineWidth)
       {       
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->line_width);
	 offset += 4;
       }

     if (valuemask & GCLineStyle)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->line_style);
	 offset += 4;
       }

     if (valuemask & GCCapStyle)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->cap_style);
	 offset += 4;
       }

     if (valuemask & GCJoinStyle)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->join_style);
	 offset += 4;
       }

     if (valuemask & GCFillStyle)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->fill_style);
	 offset += 4;
       }

     if (valuemask & GCFillRule)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->fill_rule);
         offset += 4;
       }

     if (valuemask & GCTile)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->tile);
	 offset += 4;
       }     

     if (valuemask & GCStipple)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->stipple);
	 offset += 4;
       }

     if (valuemask & GCTileStipXOrigin)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->ts_x_origin);
	 offset += 4;
       }

     if (valuemask & GCTileStipYOrigin)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->ts_y_origin);
	 offset += 4;
       }

     if (valuemask & GCFont)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->font);
	 offset += 4;
       }

     if (valuemask & GCSubwindowMode)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->subwindow_mode);
	 offset += 4;
       }

     if (valuemask & GCGraphicsExposures)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->graphics_exposures);
	 offset += 4;
       }

     if (valuemask & GCClipXOrigin)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->clip_x_origin);
	 offset += 4;
       }

     if (valuemask & GCClipYOrigin)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->clip_y_origin);
	 offset += 4;
       }

     if (valuemask & GCClipMask)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->clip_mask);
	 offset += 4;
       }

     if (valuemask & GCDashOffset)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->dash_offset);
	 offset += 4;
       }

     if (valuemask & GCDashList)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->dashes);
	 offset += 4;
       }

     if (valuemask & GCArcMode)
       {
	 numvalues++;
	 ISetLong(&temp_buf[offset], values->arc_mode);
	 offset += 4;
       }     

     CreateGCRequest.reqType  = X_CreateGC;
     ISetShort((unsigned char *)&CreateGCRequest.length, 4 + numvalues);
     ISetLong((unsigned char *)&CreateGCRequest.gc, id);
     ISetLong((unsigned char *)&CreateGCRequest.drawable, d);
     ISetLong((unsigned char *)&CreateGCRequest.mask, valuemask);

     SendBuffer(fd, (unsigned char *)&CreateGCRequest, sz_xCreateGCReq);
     SendBuffer(fd, &temp_buf[0], numvalues << 2);
     ++(*seqno);

     return id;
}


Cursor XMOVECreatePixmapCursor(fd, seqno, id, source, mask, foreground,
			       background, x, y)
int fd;
unsigned short *seqno;
Cursor id;
Pixmap source, mask;
XColor *foreground, *background;
unsigned int x, y;
{
     xCreateCursorReq CreateCursorRequest;

     Dprintf(("in XMOVECreatePixmapCursor\n"));

     CreateCursorRequest.reqType   = X_CreateCursor;
     ISetShort((unsigned char *)&CreateCursorRequest.length, 8);
     ISetLong((unsigned char *)&CreateCursorRequest.cid, id);
     ISetLong((unsigned char *)&CreateCursorRequest.source, source);
     ISetLong((unsigned char *)&CreateCursorRequest.mask, mask);
     ISetShort((unsigned char *)&CreateCursorRequest.foreRed, foreground->red);
     ISetShort((unsigned char *)&CreateCursorRequest.foreGreen, foreground->green);
     ISetShort((unsigned char *)&CreateCursorRequest.foreBlue, foreground->blue);
     ISetShort((unsigned char *)&CreateCursorRequest.backRed, background->red);
     ISetShort((unsigned char *)&CreateCursorRequest.backGreen, background->green);
     ISetShort((unsigned char *)&CreateCursorRequest.backBlue, background->blue);
     ISetShort((unsigned char *)&CreateCursorRequest.x, x);
     ISetShort((unsigned char *)&CreateCursorRequest.y, y);

     SendBuffer(fd, (unsigned char *)&CreateCursorRequest, sz_xCreateCursorReq);
     ++(*seqno);

     return id;
}

void
XMOVEFreeGC (fd, seqno, gc)
int fd;
unsigned short *seqno;
GContext gc;
{
     xResourceReq FreeGCRequest;

     Dprintf(("in XMOVEFreeGC\n"));
     FreeGCRequest.reqType = X_FreeGC;
     ISetShort((unsigned char *)&FreeGCRequest.length, 2);
     ISetLong((unsigned char *)&FreeGCRequest.id, gc);

     SendBuffer(fd, (unsigned char *)&FreeGCRequest, 8);
     ++(*seqno);
}

void
XMOVEFreePixmap (fd, seqno, pixmap)
int fd;
unsigned short *seqno;
Pixmap pixmap;
{
     xResourceReq FreePixmapRequest;

     Dprintf(("in XMOVEFreePixmap\n"));
     FreePixmapRequest.reqType = X_FreePixmap;
     ISetShort((unsigned char *)&FreePixmapRequest.length, 2);
     ISetLong((unsigned char *)&FreePixmapRequest.id, pixmap);

     SendBuffer(fd, (unsigned char *)&FreePixmapRequest, 8);
     ++(*seqno);
}

void
XMOVEMapWindow (fd, seqno, w)
int fd;
unsigned short *seqno;
Window w;
{
     xResourceReq MapWindowRequest;

     Dprintf(("in XMOVEMapWindow mapping 0x%x\n",w));
     MapWindowRequest.reqType = X_MapWindow;
     ISetShort((unsigned char *)&MapWindowRequest.length, 2);
     ISetLong((unsigned char *)&MapWindowRequest.id, w);

     SendBuffer(fd, (unsigned char *)&MapWindowRequest, 8);
     ++(*seqno);
}

Window
XMOVECreateWindow(fd, seqno, id, parent, x, y, width, height, border_width,
		  depth, class, visualid, valuemask, attributes)
int fd;
unsigned short *seqno;
Window id, parent;
int x,y;
unsigned int width, height, border_width;
int depth;
unsigned int class;
VisualID visualid;
unsigned long valuemask;
XSetWindowAttributes *attributes;
{
     xCreateWindowReq CreateWindowRequest;
     unsigned char tempbuf[15*4], *bufptr = tempbuf;
     unsigned long realvaluemask = 0, valuescount = 0;

     Dprintf(("in XMOVECreateWindow\n"));

     if (valuemask & CWBackPixmap) {
	  ISetLong(bufptr, attributes->background_pixmap);
	  realvaluemask |= CWBackPixmap;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBackPixel) {
	  ISetLong(bufptr, attributes->background_pixel);
	  realvaluemask |= CWBackPixel;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBorderPixmap) {
	  ISetLong(bufptr, attributes->border_pixmap);
	  realvaluemask |= CWBorderPixmap;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBorderPixel) {
	  ISetLong(bufptr, attributes->border_pixel);
	  realvaluemask |= CWBorderPixel;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBitGravity) {
	  ISetLong(bufptr, attributes->bit_gravity);
	  realvaluemask |= CWBitGravity;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWWinGravity) {
	  ISetLong(bufptr, attributes->win_gravity);
	  realvaluemask |= CWWinGravity;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBackingStore) {
	  ISetLong(bufptr, attributes->backing_store);
	  realvaluemask |= CWBackingStore;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBackingPlanes) {
	  ISetLong(bufptr, attributes->backing_planes);
	  realvaluemask |= CWBackingPlanes;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWBackingPixel) {
	  ISetLong(bufptr, attributes->backing_pixel);
	  realvaluemask |= CWBackingPixel;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWOverrideRedirect) {
	  ISetLong(bufptr, attributes->override_redirect);
	  realvaluemask |= CWOverrideRedirect;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWSaveUnder) {
	  ISetLong(bufptr, attributes->save_under);
	  realvaluemask |= CWSaveUnder;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWEventMask) {
	  ISetLong(bufptr, attributes->event_mask);
	  realvaluemask |= CWEventMask;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWDontPropagate) {
	  ISetLong(bufptr, attributes->do_not_propagate_mask);
	  realvaluemask |= CWDontPropagate;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWColormap) {
	  ISetLong(bufptr, attributes->colormap);
	  realvaluemask |= CWColormap;
	  valuescount++;
	  bufptr += 4;
     }

     if (valuemask & CWCursor) {
	  ISetLong(bufptr, attributes->cursor);
	  realvaluemask |= CWCursor;
	  valuescount++;
	  bufptr += 4;
     }

     Dprintf (("XMOVECreateWindow: (%d,%d)-(%d,%d), with %d attributes\n",
	     x, y, x+width, y+height, valuescount));

     CreateWindowRequest.reqType     = X_CreateWindow;
     CreateWindowRequest.depth       = depth;
     ISetShort((unsigned char *)&CreateWindowRequest.length, 8 + valuescount);
     ISetLong((unsigned char *)&CreateWindowRequest.wid, id);
     ISetLong((unsigned char *)&CreateWindowRequest.parent, parent);
     ISetShort((unsigned char *)&CreateWindowRequest.x, x);
     ISetShort((unsigned char *)&CreateWindowRequest.y, y);
     ISetShort((unsigned char *)&CreateWindowRequest.width, width);
     ISetShort((unsigned char *)&CreateWindowRequest.height, height);
     ISetShort((unsigned char *)&CreateWindowRequest.borderWidth, border_width);
     ISetShort((unsigned char *)&CreateWindowRequest.class, class);
     ISetLong((unsigned char *)&CreateWindowRequest.visual, visualid);
     ISetLong((unsigned char *)&CreateWindowRequest.mask, realvaluemask);

     SendBuffer(fd, (unsigned char *)&CreateWindowRequest, sz_xCreateWindowReq);
     SendBuffer(fd, tempbuf, valuescount<<2);
     ++(*seqno);
     
     return id;
}

void
XMOVEGrabButton(fd, seqno, button, modifiers, grab_window, owner_events,
		event_mask, pointer_mode, keyboard_mode, confine_to, curs)
int fd;
unsigned short *seqno;
unsigned int modifiers; /* CARD16 */
unsigned int button; /* CARD8 */
Window grab_window;
Bool owner_events;
unsigned int event_mask; /* CARD16 */
int pointer_mode, keyboard_mode;
Window confine_to;
Cursor curs;
{
     xGrabButtonReq GrabButtonRequest;

     Dprintf(("in XMOVEGrabButton\n"));

     GrabButtonRequest.reqType      = X_GrabButton;
     GrabButtonRequest.ownerEvents  = owner_events;
     ISetShort((unsigned char *)&GrabButtonRequest.length, 6);
     ISetLong((unsigned char *)&GrabButtonRequest.grabWindow, grab_window);
     ISetShort((unsigned char *)&GrabButtonRequest.eventMask, event_mask);
     GrabButtonRequest.pointerMode  = pointer_mode;
     GrabButtonRequest.keyboardMode = keyboard_mode;
     ISetLong((unsigned char *)&GrabButtonRequest.confineTo, confine_to);
     ISetLong((unsigned char *)&GrabButtonRequest.cursor, curs);
     GrabButtonRequest.button       = button;
     ISetShort((unsigned char *)&GrabButtonRequest.modifiers, modifiers);

     SendBuffer(fd, (unsigned char *)&GrabButtonRequest, sz_xGrabButtonReq);
     ++(*seqno);
}

void
XMOVEGrabKey(fd, seqno, key, modifiers, grab_window, owner_events, 
	     pointer_mode, keyboard_mode)
int fd;
unsigned short *seqno;
int key;
unsigned int modifiers;
Window grab_window;
Bool owner_events;
int pointer_mode, keyboard_mode;
{
     xGrabKeyReq GrabKeyRequest;

     Dprintf(("in XMOVEGrabKey\n"));

     GrabKeyRequest.reqType      = X_GrabKey;
     GrabKeyRequest.ownerEvents  = owner_events;
     ISetShort((unsigned char *)&GrabKeyRequest.length, 4);
     ISetLong((unsigned char *)&GrabKeyRequest.grabWindow, grab_window);
     ISetShort((unsigned char *)&GrabKeyRequest.modifiers, modifiers);
     GrabKeyRequest.key          = key;
     GrabKeyRequest.pointerMode  = pointer_mode;
     GrabKeyRequest.keyboardMode = keyboard_mode;

     SendBuffer(fd, (unsigned char *)&GrabKeyRequest, sz_xGrabKeyReq);
     ++(*seqno);
}

Global void
XMOVEChangeProperty(fd, seqno, w, property, type, format, mode,
		    data, nelements)
int fd;
unsigned short *seqno;
Window w;
Atom property, type;
int format;
int mode;
unsigned char *data;
int nelements;
{
     int datalen = nelements * (format / 8);
     int totlen  = ROUNDUP4(datalen);

     xChangePropertyReq ChangePropertyRequest;

     ChangePropertyRequest.reqType  = X_ChangeProperty;
     ChangePropertyRequest.mode     = mode;
     ISetShort((unsigned char *)&ChangePropertyRequest.length, 6 + totlen/4);
     ISetLong((unsigned char *)&ChangePropertyRequest.window, w);
     ISetLong((unsigned char *)&ChangePropertyRequest.property, property);
     ISetLong((unsigned char *)&ChangePropertyRequest.type, type);
     ChangePropertyRequest.format   = format;
     ISetLong((unsigned char *)&ChangePropertyRequest.nUnits, nelements);

     Dprintf(("in XMOVEChangeProperty\n"));

     SendBuffer(fd, (unsigned char *)&ChangePropertyRequest, sz_xChangePropertyReq);
     SendBuffer(fd, data, datalen);
     SendBuffer(fd, (unsigned char *)&ChangePropertyRequest, totlen - datalen); /*pad*/
     
     ++(*seqno);
}


Status
XMOVEAllocColor(fd, seqno, cmap, def)
int fd;
unsigned short *seqno;
Colormap cmap;
XColor *def;
{
     xAllocColorReq          AllocColorRequest;
     xAllocColorReply        AllocColorReply;

     Dprintf(("in XMOVEAllocColor\n"));

     AllocColorRequest.reqType = X_AllocColor;
     ISetShort((unsigned char *)&AllocColorRequest.length, 4);
     ISetLong((unsigned char *)&AllocColorRequest.cmap, cmap);
     ISetShort((unsigned char *)&AllocColorRequest.red, def->red);
     ISetShort((unsigned char *)&AllocColorRequest.green, def->green);
     ISetShort((unsigned char *)&AllocColorRequest.blue, def->blue);
     
     SendBuffer(fd, (unsigned char *)&AllocColorRequest, sz_xAllocColorReq);

     if (!ReceiveReply(fd, (unsigned char *)&AllocColorReply, 32, ++(*seqno)))
	  return False;

     def->red   = IShort((unsigned char *)&AllocColorReply.red);
     def->green = IShort((unsigned char *)&AllocColorReply.green);
     def->blue  = IShort((unsigned char *)&AllocColorReply.blue);
     def->pixel = ILong((unsigned char *)&AllocColorReply.pixel);
     
     return True;
}


Status
XMOVEAllocNamedColor(fd, seqno, cmap, colorname, hard_def, exact_def)
int fd;
unsigned short *seqno;
Colormap cmap;
char *colorname;
XColor *hard_def;
XColor *exact_def;
{
     xAllocNamedColorReq          AllocNamedColorRequest;
     xAllocNamedColorReply        AllocNamedColorReply;
     unsigned int length = strlen(colorname);

     Dprintf(("in XMOVEAllocNamedColor\n"));

     AllocNamedColorRequest.reqType = X_AllocNamedColor;
     ISetShort((unsigned char *)&AllocNamedColorRequest.length, 3 + (length+3)/4);
     ISetLong((unsigned char *)&AllocNamedColorRequest.cmap, cmap);
     ISetShort((unsigned char *)&AllocNamedColorRequest.nbytes, length);
     
     SendBuffer(fd, (unsigned char *)&AllocNamedColorRequest, sz_xAllocNamedColorReq);
     SendBuffer(fd, (unsigned char *)colorname, length);
     SendBuffer(fd, (unsigned char *)&AllocNamedColorRequest, ROUNDUP4(length)-length);
     
     if (!ReceiveReply(fd, (unsigned char *)&AllocNamedColorReply,
		       32, ++(*seqno)))
	  return False;
     
     exact_def->red   = IShort((unsigned char *)&AllocNamedColorReply.exactRed);
     exact_def->green = IShort((unsigned char *)&AllocNamedColorReply.exactGreen);
     exact_def->blue  = IShort((unsigned char *)&AllocNamedColorReply.exactBlue);
     exact_def->pixel = ILong((unsigned char *)&AllocNamedColorReply.pixel);
     
     hard_def->red   = IShort((unsigned char *)&AllocNamedColorReply.screenRed);
     hard_def->green = IShort((unsigned char *)&AllocNamedColorReply.screenGreen);
     hard_def->blue  = IShort((unsigned char *)&AllocNamedColorReply.screenBlue);
     hard_def->pixel = ILong((unsigned char *)&AllocNamedColorReply.pixel);
     
     return True;
}


void
XMOVEStoreColor(fd, seqno, cmap, def)
int fd;
unsigned short *seqno;
Colormap cmap;
XColor *def;
{
     xStoreColorsReq          StoreColorRequest;
     xColorItem               citem;

     Dprintf(("in XMOVEStoreColor\n"));

     StoreColorRequest.reqType = X_StoreColors;
     ISetShort((unsigned char *)&StoreColorRequest.length, 5);
     ISetLong((unsigned char *)&StoreColorRequest.cmap, cmap);
     
     ISetLong((unsigned char *)&citem.pixel, def->pixel);
     ISetShort((unsigned char *)&citem.red, def->red);
     ISetShort((unsigned char *)&citem.green, def->green);
     ISetShort((unsigned char *)&citem.blue, def->blue);
     citem.flags = def->flags;
     
     SendBuffer(fd, (unsigned char *)&StoreColorRequest, sz_xStoreColorsReq);
     SendBuffer(fd, (unsigned char *)&citem, sizeof(xColorItem));
     ++(*seqno);
}


void
XMOVEStoreNamedColor(fd, seqno, cmap, name, pixel, flags)
int fd;
unsigned short *seqno;
Colormap cmap;
char *name;
unsigned long pixel;
int flags;
{
     xStoreNamedColorReq     StoreNamedColorRequest;
     int length = strlen(name);

     Dprintf(("in XMOVEStoreNamedColor\n"));

     StoreNamedColorRequest.reqType = X_StoreNamedColor;
     StoreNamedColorRequest.flags   = flags;
     ISetShort((unsigned char *)&StoreNamedColorRequest.length, 4 + ROUNDUP4(length)/4);
     ISetLong((unsigned char *)&StoreNamedColorRequest.cmap, cmap);
     ISetLong((unsigned char *)&StoreNamedColorRequest.pixel, pixel);
     ISetShort((unsigned char *)&StoreNamedColorRequest.nbytes, length);
     
     SendBuffer(fd, (unsigned char *)&StoreNamedColorRequest, sz_xStoreNamedColorReq);
     SendBuffer(fd, (unsigned char *)name, length);
     SendBuffer(fd, (unsigned char *)&StoreNamedColorRequest, ROUNDUP4(length)-length);
     ++(*seqno);
}


void
XMOVEQueryColor(fd, seqno, cmap, def)
int fd;
unsigned short *seqno;
Colormap cmap;
XColor *def;
{
     xQueryColorsReq          QueryColorRequest;
     xQueryColorsReply        QueryColorReply;
     xrgb color;
     unsigned char pixel[4];

     Dprintf(("in XMOVEQueryColor\n"));

     QueryColorRequest.reqType = X_QueryColors;
     ISetShort((unsigned char *)&QueryColorRequest.length, 3);
     ISetLong((unsigned char *)&QueryColorRequest.cmap, cmap);
     ISetLong(pixel, def->pixel);
     
     SendBuffer(fd, (unsigned char *)&QueryColorRequest, sz_xQueryColorsReq);
     SendBuffer(fd, pixel, 4);
     
     if (!ReceiveReply(fd, (unsigned char *)&QueryColorReply,
			32, ++(*seqno))) {
	  printf("call to XMOVEQueryColor() caused error from server\n");
	  return;
     }
     
     ReceiveBuffer(fd, (unsigned char *)&color, 8);

     def->red   = IShort((unsigned char *)&color.red);
     def->blue  = IShort((unsigned char *)&color.blue);
     def->green = IShort((unsigned char *)&color.green);
     def->flags = DoRed | DoGreen | DoBlue;

     return;
}


Status
XMOVEAllocColorCells(fd, seqno, cmap, contig, masks, nplanes, pixels, ncolors)
int fd;
unsigned short *seqno;
Colormap cmap;
Bool contig;
unsigned long *masks;
unsigned int nplanes;
unsigned long *pixels;
unsigned int ncolors;
{
     xAllocColorCellsReq      AllocColorCellsRequest;
     xAllocColorCellsReply    AllocColorCellsReply;

     int counter, nPixels, nMasks;

     Dprintf(("in XMOVEAllocColorCells\n"));

     AllocColorCellsRequest.reqType    = X_AllocColorCells;
     AllocColorCellsRequest.contiguous = contig;
     ISetShort((unsigned char *)&AllocColorCellsRequest.length, 3);
     ISetLong((unsigned char *)&AllocColorCellsRequest.cmap, cmap);
     ISetShort((unsigned char *)&AllocColorCellsRequest.colors, ncolors);
     ISetShort((unsigned char *)&AllocColorCellsRequest.planes, nplanes);
     
     SendBuffer(fd, (unsigned char *)&AllocColorCellsRequest, sz_xAllocColorCellsReq);
     
     if (!ReceiveReply(fd, (unsigned char *)&AllocColorCellsReply,
			32, ++(*seqno)))
	  return False;
     
     nPixels = IShort((unsigned char *)&AllocColorCellsReply.nPixels);
     nMasks = IShort((unsigned char *)&AllocColorCellsReply.nMasks);

     ReceiveBuffer(fd, (unsigned char *)pixels, 4*nPixels);
     ReceiveBuffer(fd, (unsigned char *)masks,  4*nMasks);

     for (counter = 0; counter < nPixels; counter++)
	  ISetLong((unsigned char *)&pixels[counter], pixels[counter]);

     for (counter = 0; counter < nMasks; counter++)
	  ISetLong((unsigned char *)&masks[counter], masks[counter]);

     return True;
}


Colormap
XMOVECreateColormap(fd, seqno, mid, w, visual, alloc)
int fd;
unsigned short *seqno;
Pixmap mid;
Window w;
VisualID visual;
int alloc;
{
     xCreateColormapReq CreateColormapRequest;

     Dprintf(("in XMOVECreateColormap\n"));

     CreateColormapRequest.reqType = X_CreateColormap;
     CreateColormapRequest.alloc   = alloc;
     ISetShort((unsigned char *)&CreateColormapRequest.length, 4);
     ISetLong((unsigned char *)&CreateColormapRequest.mid, mid);
     ISetLong((unsigned char *)&CreateColormapRequest.window, w);
     ISetLong((unsigned char *)&CreateColormapRequest.visual, visual);

     SendBuffer(fd, (unsigned char *)&CreateColormapRequest, sz_xCreateColormapReq);
     ++(*seqno);

     return mid;
}


Status
XMOVELookupColor(fd, seqno, cmap, spec, def, scr)
int fd;
unsigned short *seqno;
Colormap cmap;
char *spec;
XColor *def, *scr;
{
     xLookupColorReq          LookupColorRequest;
     xLookupColorReply        LookupColorReply;
     int length = strlen(spec);
     
     Dprintf(("in XMOVELookupColor\n"));

     LookupColorRequest.reqType = X_LookupColor;
     ISetShort((unsigned char *)&LookupColorRequest.length, 3 + ROUNDUP4(length)/4);
     ISetLong((unsigned char *)&LookupColorRequest.cmap, cmap);
     ISetShort((unsigned char *)&LookupColorRequest.nbytes, length);
     
     SendBuffer(fd, (unsigned char *)&LookupColorRequest, sz_xLookupColorReq);
     SendBuffer(fd, (unsigned char *)spec, length);
     SendBuffer(fd, (unsigned char *)&LookupColorRequest, ROUNDUP4(length)-length);

     if (!ReceiveReply(fd, (unsigned char *)&LookupColorReply, 32, ++(*seqno)))
	  return False;
     
     def->red   = IShort((unsigned char *)&LookupColorReply.exactRed);
     def->green = IShort((unsigned char *)&LookupColorReply.exactGreen);
     def->blue  = IShort((unsigned char *)&LookupColorReply.exactBlue);

     scr->red   = IShort((unsigned char *)&LookupColorReply.screenRed);
     scr->green = IShort((unsigned char *)&LookupColorReply.screenGreen);
     scr->blue  = IShort((unsigned char *)&LookupColorReply.screenBlue);

     return True;
}

char *
XMOVEGetAtomName(fd, seqno, atom)
int fd;
unsigned short *seqno;
Atom atom;
{
     xResourceReq          req;
     xGetAtomNameReply     rep;
     char *name;
     int namelen;

     Dprintf(("in XMOVEGetAtomName\n"));

     req.reqType = X_GetAtomName;
     ISetShort((unsigned char *)&req.length, 2);
     ISetLong((unsigned char *)&req.id, atom);
     
     SendBuffer(fd, (unsigned char *)&req, sz_xResourceReq);

     if (!ReceiveReply(fd, (unsigned char *)&rep, 32, ++(*seqno)))
	  return NULL;

     namelen = IShort((unsigned char *)&rep.nameLength);
     name = malloc(namelen + 1);

     ReceiveBuffer(fd, (unsigned char *)name, namelen);
     name[namelen] = '\0';

     /* use req as scratch space for pad. its contents aren't needed anymore */
     ReceiveBuffer(fd, (unsigned char *)&req, ILong((unsigned char *)&rep.length)*4 - namelen);
     
     return name;
}


Global int
XMOVEGetProperty(fd, seqno, w, property, offset, length, delete,
		 req_type, actual_type, actual_format, nitems, bytesafter, prop)
int fd;
unsigned short *seqno;
Window w;
Atom property;
Bool delete;
Atom req_type;
Atom *actual_type;
int *actual_format;
long offset, length;
unsigned long *nitems;
unsigned long *bytesafter;
unsigned char **prop;
{
     xGetPropertyReq req;
     xGetPropertyReply reply;
     int rep_length;
     
     req.reqType    = X_GetProperty;
     req.delete     = delete;
     ISetShort((unsigned char *)&req.length, 6);
     ISetLong((unsigned char *)&req.window, w);
     ISetLong((unsigned char *)&req.property, property);
     ISetLong((unsigned char *)&req.type, req_type);
     ISetLong((unsigned char *)&req.longOffset, offset);
     ISetLong((unsigned char *)&req.longLength, length);

     SendBuffer(fd, (unsigned char *)&req, 24);
     if (!ReceiveReply(fd, (unsigned char *)&reply, 32, ++(*seqno)))
	  return 1;

     *actual_format = reply.format;
     *actual_type   = ILong((unsigned char *)&reply.propertyType);
     *nitems        = ILong((unsigned char *)&reply.nItems);
     *bytesafter    = ILong((unsigned char *)&reply.bytesAfter);

     rep_length = *nitems * (reply.format / 8);
     *prop = malloc(rep_length);

     ReceiveBuffer(fd, *prop, rep_length);
     ReceiveBuffer(fd, (unsigned char *)&req, ROUNDUP4(rep_length)-rep_length);

     return Success;
}


Global void
XMOVEUnmapWindow(fd, seqno, w)
int fd;
unsigned short *seqno;
Window w;
{
     xResourceReq req;

     Dprintf(("in XMOVEUnmapWindow\n"));

     req.reqType  = X_UnmapWindow;
     ISetShort((unsigned char *)&req.length, 2);
     ISetLong((unsigned char *)&req.id, w);

     SendBuffer(fd, (unsigned char *)&req, sz_xResourceReq);
     ++(*seqno);

     return;
}


/* note this version doesn't provide return values. */

Global void
XMOVEGetInputFocus(fd, seqno)
int fd;
unsigned short *seqno;
{
     xReq req;
     xGetInputFocusReply rep;

     req.reqType = X_GetInputFocus;
     ISetShort((unsigned char *)&req.length, 1);

     SendBuffer(fd, (unsigned char *)&req, sz_xReq);
     ReceiveReply(fd, (unsigned char *)&rep, 32, ++(*seqno));
}

/* check to see if an error has been received by sending
 * a GetInputFocus request and seeing if an error comes before
 * the reply
 */

Global Bool
XMOVEVerifyRequest(fd, seqno)
int fd;
unsigned short *seqno;
{
     Bool status;
     xReq req;
     xGetInputFocusReply rep;

     req.reqType = X_GetInputFocus;
     ISetShort((unsigned char *)&req.length, 1);

     SendBuffer(fd, (unsigned char *)&req, sz_xReq);
     ReceiveReplyWError(fd, (unsigned char *)&rep, 32, ++(*seqno), &status);

     return !status;
}

/* XMOVEReceiveReplyOrError is used to sync up with the
 * server, but if there was an error, report it.
 * ReceiveReplyOrError will return -1 if something strange happens,
 * 0 if we're done, or 1, indicating that error now points to an error.
 * *NOT* re-entrant
 */

Global
XMOVEReceiveReplyOrError(fd, seqno, error)
int fd;
unsigned short *seqno;
unsigned short *error;
{
   static int in_progress = 0;
   static u_short sync_seqno;
   
   int retval;
   xGetInputFocusReply rep;
   unsigned char *repptr = (unsigned char *)&rep;
   unsigned short curseqno;
   
   if (in_progress == 0) {
       xReq req;

       req.reqType = X_GetInputFocus;
       ISetShort((unsigned char *)&req.length, 1);
       SendBuffer(fd, (unsigned char *)&req, sz_xReq);
       sync_seqno = ++(*seqno);

       in_progress = 1;
   }

   while (1) {
       retval = ReceiveBuffer(fd, repptr, 32);
       curseqno = IShort(repptr+2);
       
       if (curseqno == sync_seqno) {
	   in_progress = 0;
	   
	   if (IByte(repptr) == 1)
	       return 0;
	   else
	       return -1;
       }
       
       if (IByte(repptr) == 0) {
	   *error = curseqno;
	   return 1;
       }
   }
}

Global Status
XMOVEQueryTree(int fd, u_short *seqno, Window w,
	       Window **children_return, u_int *nchildren_return)
{
     xResourceReq    req;
     xQueryTreeReply rep;

     req.reqType = X_QueryTree;
     ISetShort((u_char *)&req.length, 2);
     ISetLong((u_char *)&req.id, w);

     SendBuffer(fd, (u_char *)&req, sizeof(xResourceReq));
     if (!ReceiveReply(fd, (u_char *)&rep, 32, ++(*seqno))) {
	  *nchildren_return = 0;
	  *children_return = NULL;

	  return False;
     }
     
     *nchildren_return = IShort((u_char *)&rep.nChildren);
     *children_return = (Window *)malloc(*nchildren_return << 2);
     ReceiveBuffer(fd, (u_char *)*children_return, *nchildren_return << 2);

     return True;
}

Global void
XMOVERaiseWindow(int fd, u_short *seqno, Window w)
{
     xConfigureWindowReq req;
     u_char buf[4];

     req.reqType = X_ConfigureWindow;
     ISetShort((u_char *)&req.length, 4);
     ISetLong ((u_char *)&req.window, w);
     ISetShort((u_char *)&req.mask,   CWStackMode);
     ISetLong(&buf[0],                Above);

     SendBuffer(fd, (u_char *)&req, sz_xConfigureWindowReq);
     SendBuffer(fd, &buf[0], 4);
     ++(*seqno);
}

Global void
XMOVESetClipRectangles(int fd, u_short *seqno, GContext gc,
		       int clip_x_origin, int clip_y_origin,
		       void *rectangles, int n, int ordering)
{
	xSetClipRectanglesReq req;
	
	req.reqType = X_SetClipRectangles;
	ISetByte ((u_char *)&req.ordering, ordering);
	ISetShort((u_char *)&req.length,   3 + 2*n);
	ISetLong ((u_char *)&req.gc,       gc);
	ISetShort((u_char *)&req.xOrigin,  clip_x_origin);
	ISetShort((u_char *)&req.yOrigin,  clip_y_origin);

	SendBuffer(fd, (u_char *)&req, sz_xSetClipRectanglesReq);
	SendBuffer(fd, rectangles, 8*n);
	++(*seqno);
}

Global void
XMOVESetCloseDownMode(int fd, u_short *seqno, int shutdownmode)
{
	xSetCloseDownModeReq req;

	req.reqType = X_SetCloseDownMode;
	ISetByte ((u_char *)&req.mode,   shutdownmode);
	ISetShort((u_char *)&req.length, 1);

	SendBuffer(fd, (u_char *)&req, sz_xSetCloseDownModeReq);
	++(*seqno);
}
