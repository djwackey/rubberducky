/* Bindings for the librtmp library.
 */

/**
 * TODO take packet init off the stack and use a slab-type allocator
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h> // for htonl

#include <librtmp/amf.h>

#include "mediaserver.h"
#include "amf.h"

// yanked wholesale from librtmp
#define SAVC(x) static const AVal av_##x = AVC(#x)
// client connect parms
SAVC(connect);
SAVC(app);
SAVC(flashVer);
SAVC(tcUrl);
SAVC(pageUrl);
SAVC(audioCodecs);
SAVC(videoCodecs);
SAVC(objectEncoding);

// other netconnection commands
SAVC(releaseStream);
SAVC(FCPublish);
SAVC(FCUnpublish);
SAVC(createStream);
SAVC(deleteStream);
SAVC(publish);
SAVC(play);

#define STR2AVAL(av,str)	av.av_val = str; av.av_len = strlen(av.av_val)

static int set_peer_bw(rtmp *rtmp)
{
    uint8_t pbuf[RTMP_MAX_HEADER_SIZE+5] = {0};
    amf_write_i32(pbuf + RTMP_MAX_HEADER_SIZE, pbuf + RTMP_MAX_HEADER_SIZE + 4, 0x0fffffff);
    pbuf[RTMP_MAX_HEADER_SIZE + 4] = 2;
    rtmp_packet packet = {
        .chunk_id = 0x02,
        .msg_id = 0,
        .msg_type = 0x06,
        .timestamp = 0,
        .size = sizeof(pbuf) - RTMP_MAX_HEADER_SIZE,
        .body = pbuf + RTMP_MAX_HEADER_SIZE
    };
    fprintf(stdout, "sending clientbw, rx: %d, tx %d\n", rtmp->rx, rtmp->tx);
    return rtmp_send(rtmp, &packet);
}

static int window_ack_size(rtmp *rtmp)
{
    uint8_t pbuf[RTMP_MAX_HEADER_SIZE + 4] = { 0 };
    amf_write_i32(pbuf + RTMP_MAX_HEADER_SIZE, pbuf + RTMP_MAX_HEADER_SIZE + 4, 0x0fffffff);
    rtmp_packet packet = {
        .chunk_id = 0x02,
        .msg_id = 0,
        .msg_type = 0x05,
        .timestamp = 0,
        .size = sizeof(pbuf) - RTMP_MAX_HEADER_SIZE,
        .body = pbuf + RTMP_MAX_HEADER_SIZE
    };
    return rtmp_send(rtmp, &packet);
}

static int send_ping(rtmp *rtmp)
{
    time_t now = time(NULL);
    uint8_t pbuf[RTMP_MAX_HEADER_SIZE+4];
    amf_write_i32(pbuf + RTMP_MAX_HEADER_SIZE, pbuf + sizeof(pbuf), now);
    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE);
    rtmp_packet packet = {
        .chunk_id = 0x02,
        .msg_id = 0,
        .msg_type = 0x04,
        .timestamp = 0,
        .size = sizeof(pbuf) - RTMP_MAX_HEADER_SIZE,
        .body = pbuf + RTMP_MAX_HEADER_SIZE
    };
    return rtmp_send(rtmp, &packet);
}

//XXX figure out just WTF the stream id is used for
static int send_result(rtmp *rtmp, double txn, double stream_id)
{
    uint8_t pbuf[128], *end = pbuf+sizeof(pbuf), *enc = pbuf+RTMP_MAX_HEADER_SIZE, *foo;
    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE);
    enc = amf_write_str(enc, end, "_result");
    enc = amf_write_dbl(enc, end, txn);
    *enc++ = AMF_NULL; //command object
    enc = amf_write_dbl(enc, end, stream_id); // IS THIS A HEADER?!?
    foo = pbuf+RTMP_MAX_HEADER_SIZE;
    rtmp_packet packet = {
        .chunk_id = 0x03,
        .msg_type = 0x14,
        .msg_id = 0,
        .timestamp = 0,
        .size = enc - foo,
        .body = foo
    };
    return rtmp_send(rtmp, &packet);
}

static int send_onbw_done(rtmp *rtmp)
{
    // i have never actually seen a flash client make use of this.
    uint8_t pbuf[128], *end = pbuf+sizeof(pbuf), *enc = pbuf+RTMP_MAX_HEADER_SIZE, *foo;
    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE); // to shut up valgrind
    enc = amf_write_str(enc, end, "onBWDone");
    enc = amf_write_dbl(enc, end, 0);
    *enc++ = AMF_NULL; // command object
    foo = pbuf+RTMP_MAX_HEADER_SIZE;
    rtmp_packet packet = {
        .chunk_id = 0x03,
        .msg_type = 0x14,
        .msg_id = 0,
        .timestamp = 0,
        .size = enc - foo,
        .body = foo
    };
    return rtmp_send(rtmp, &packet);
}

static int send_cxn_resp(rtmp *rtmp, double txn)
{
    rtmp_packet packet;
  uint8_t pbuf[384], *pend = pbuf+sizeof(pbuf), *enc;
    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE);
  AMFObject obj;
  AMFObjectProperty p, op;

    packet.chunk_id = 0x03; // control channel
    packet.chunk_type = CHUNK_MEDIUM;
    packet.msg_type = 0x14;
    packet.msg_id = 0;
    packet.timestamp = 0;
    packet.body = pbuf + RTMP_MAX_HEADER_SIZE;

    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE);
  enc = amf_write_str(packet.body, pend, "_result");
  enc = amf_write_dbl(enc, pend, txn);
  *enc++ = AMF_OBJECT;

  enc = amf_write_str_kv(enc, pend, "fmsVer", "FMS/3,5,1,525");
  enc = amf_write_dbl_kv(enc, pend, "capabilities", 31.0);
  enc = amf_write_dbl_kv(enc, pend, "mode", 1.0);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

  *enc++ = AMF_OBJECT;

  enc = amf_write_str_kv(enc, pend, "level", "status");
  enc = amf_write_str_kv(enc, pend, "code", "NetConnection.Connect.Success");
  enc = amf_write_str_kv(enc, pend, "description", "Connection succeeded.");
  enc = amf_write_dbl_kv(enc, pend, "objectEncoding", rtmp->encoding);
  STR2AVAL(p.p_name, "version");
  STR2AVAL(p.p_vu.p_aval, "3,5,1,525");
  p.p_type = AMF_STRING;
  obj.o_num = 1;
  obj.o_props = &p;
  op.p_type = AMF_OBJECT;  // nested
  STR2AVAL(op.p_name, "data");
  op.p_vu.p_object = obj;
  enc = (uint8_t*)AMFProp_Encode(&op, (char*)enc, (char*)pend);
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;
  *enc++ = 0;
  *enc++ = 0;
  *enc++ = AMF_OBJECT_END;

    packet.size = enc - packet.body;
    return rtmp_send(rtmp, &packet);
}

typedef enum {publish = 0, unpublish, play} stream_cmd;
static int send_fcpublish(rtmp *rtmp, const char *streamname,
                          double txn, stream_cmd action)
{
    uint8_t pbuf[256], *end = pbuf+sizeof(pbuf), *enc = pbuf+RTMP_MAX_HEADER_SIZE, *foo;
    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE);
    const char *key, *value;
    switch (action) {
    case publish:
        key = "onFCPublish";
        value = "NetStream.Publish.Start";
        break;
    case unpublish:
        key = "onFCUnpublish";
        value = "NetStream.Unpublish.Success";
        break;
    default:
        value = "We.fucked.up.sorry";
    }

    enc = amf_write_str(enc, end, key);
    enc = amf_write_dbl(enc, end, txn);
    *enc++ = AMF_NULL; // command object

    *enc++ = AMF_OBJECT;
    enc = amf_write_str_kv(enc, end, "code", value);
    enc = amf_write_str_kv(enc, end, "description", streamname);
    *enc++ = 0;
    *enc++ = 0;
    *enc++ = AMF_OBJECT_END;

    foo = pbuf+RTMP_MAX_HEADER_SIZE;
    rtmp_packet packet = {
        .chunk_id = 0x03,
        .msg_type = 0x14,
        .msg_id = 0,
        .timestamp = 0,
        .size = enc - foo,
        .body = foo
    };

    return rtmp_send(rtmp, &packet);
}

static int send_onstatus(rtmp *rtmp, char *streamname, stream_cmd action)
{
    uint8_t pbuf[256], *end = pbuf+sizeof(pbuf), *enc = pbuf+RTMP_MAX_HEADER_SIZE, *foo;
    char tbuf[64], pubstr[64]; //XXX this might not be enough later on
    memset(pbuf, 0, RTMP_MAX_HEADER_SIZE);
    enc = amf_write_str(enc, end, "onStatus");
    enc = amf_write_dbl(enc, end, 0); // transaction id
    *enc++ = AMF_NULL; // command object

    // TODO checks to enforce string bounds here (and everywhere else)
    switch(action) {
    case publish:
        strncpy(pubstr, "NetStream.Publish.Start", sizeof(pubstr));
        snprintf(tbuf, sizeof(tbuf), "%s is now published.", streamname);
        break;
    case unpublish:
        strncpy(pubstr, "NetStream.Unpublish.Success", sizeof(pubstr));
        snprintf(tbuf, sizeof(tbuf), "%s is now unpublished.", streamname);
        break;
    case play:
        //XXX this state really should be 'play pending' or something
        //TODO send PlayPublishNotify when actually ready to play
        //TODO send Play.Reset chunk before Play.Start
        strncpy(pubstr, "NetStream.Play.Start", sizeof(pubstr));
        snprintf(tbuf, sizeof(tbuf), "%s is now published.", streamname);
        break;
    default:
        strncpy(pubstr, "oops", sizeof(pubstr));
    }

    *enc++ = AMF_OBJECT;
    enc = amf_write_str_kv(enc, end, "level", "status");
    enc = amf_write_str_kv(enc, end, "code", pubstr);
    enc = amf_write_str_kv(enc, end, "description", tbuf);
    enc = amf_write_str_kv(enc, end, "clientid", "RUBBERDUCKY"); // TODO fix
    *enc++ = 0;
    *enc++ = 0;
    *enc++ = AMF_OBJECT_END;

    foo = pbuf+RTMP_MAX_HEADER_SIZE;
    rtmp_packet packet = {
        .chunk_id = 0x04,
        .msg_type = 0x14,
        .msg_id = 0,
        .timestamp = 0,
        .size = enc - foo,
        .body = foo
    };

    return rtmp_send(rtmp, &packet);
}

static void handle_connect(rtmp *rtmp, rtmp_packet *pkt, AMFObject *obj)
{
        AMFObject cobj;
        AVal pname, pval;
        int i;
        AMFProp_GetObject(AMF_GetProp(obj, NULL, 2), &cobj);
        for(i = 0; i < cobj.o_num; i++)
        {
            pname = cobj.o_props[i].p_name;
            pval.av_val = NULL;
            pval.av_len = 0;
            if(AMF_STRING == cobj.o_props[i].p_type)
            {
                pval = cobj.o_props[i].p_vu.p_aval;// dammit, ugly
            }
            if(AVMATCH(&pname, &av_app))
            {
                char *app = malloc(pval.av_len + 1);
                if (!app) { // do something drastic!
                    fprintf(stderr, "Out of memory!\n");
                }
                strncpy(app, pval.av_val, pval.av_len);
                app[pval.av_len] = '\0'; // pval may not be nulled
                rtmp->app = app;
                fprintf(stdout, "app: %s\n", rtmp->app);
                pval.av_val = NULL;
            } else if(AVMATCH(&pname, &av_flashVer))
            {
                //rtmp->Link.flashVer = pval;
                pval.av_val = NULL;
            } else if(AVMATCH(&pname, &av_tcUrl))
            {
                fprintf(stdout, "tcUrl: %s\n", pval.av_val);
                rtmp->url = malloc(pval.av_len + 1);
                if (!rtmp->url) { // TODO something drastic
                    fprintf(stderr, "Out of memory when allocating tc_url!\n");
                    return;
                }
                strncpy(rtmp->url, pval.av_val, pval.av_len);
                rtmp->url[pval.av_len] = '\0';
                pval.av_val = NULL;
            } else if(AVMATCH(&pname, &av_pageUrl))
            {
                //rtmp->Link.pageUrl = pval;
                pval.av_val = NULL;
            } else if(AVMATCH(&pname, &av_audioCodecs))
            {
                //rtmp->m_fAudioCodecs = cobj.o_props[i].p_vu.p_number;
            } else if(AVMATCH(&pname, &av_videoCodecs))
            {
                //rtmp->m_fVideoCodecs = cobj.o_props[i].p_vu.p_number;
            } else if(AVMATCH(&pname, &av_objectEncoding))
            {
                switch((int)cobj.o_props[i].p_vu.p_number) {
                case AMF0:
                    rtmp->encoding = AMF0;
                    break;
                case AMF3:
                    rtmp->encoding = AMF3;
                    break;
                default:
                    fprintf(stderr, "Unknown AMF encoding %d\n",
                            (int)cobj.o_props[i].p_vu.p_number);
                    return; // XXX do something drastic; close cxn?
                }
                fprintf(stderr, "object encoding: AMF%d\n",
                        rtmp->encoding);
            }
            // unrecognized string
            if(pval.av_val)
            {
                // do something? log?
            }
            //rtmp->m_bSendCounter = FALSE; // for sending bytes received message
        }
}

void rtmp_invoke(rtmp *rtmp, rtmp_packet *pkt, srv_ctx *ctx)
{
    uint8_t *body = pkt->body;
    int pkt_len = pkt->size;
    double txn; // transaction id
    const char *errstr;
    AMFObject obj;
    AVal method, val;

    while (!*body){
        // for the fucked case in which: type 11 (Flex/AMF3) message
        // is received but in AMF0 format, *and* prefixed with an
        // an extra zero byte. Flash sux
        body++;
        pkt_len--;
    }
    if (body[0] != 0x02) // sanity check
    {
        errstr = "Body not 0x02";
        goto invoke_error;
    }
    if((pkt_len = AMF_Decode(&obj, (char*)body, pkt_len, FALSE)) < 0)
    {
        errstr = "Error decoding AMF object";
        goto invoke_error;
    }
    AMFProp_GetString(AMF_GetProp(&obj, NULL, 0), &method);
    txn = AMFProp_GetNumber(AMF_GetProp(&obj, NULL, 1));

    if(AVMATCH(&method, &av_connect))
    {
        window_ack_size(rtmp);
        set_peer_bw(rtmp);
        send_ping(rtmp);
        send_onbw_done(rtmp);
        handle_connect(rtmp, pkt, &obj);
        send_cxn_resp(rtmp, txn);
    } else if(AVMATCH(&method, &av_releaseStream))
    {
        send_result(rtmp, txn, pkt->msg_id);
    } else if(AVMATCH(&method, &av_FCPublish))
    {
        send_result(rtmp, txn, pkt->msg_id);
        AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &val);
        send_fcpublish(rtmp, val.av_val, txn, publish);
    } else if(AVMATCH(&method, &av_FCUnpublish))
    {
        send_result(rtmp, txn, pkt->msg_id);
        AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &val);
        send_fcpublish(rtmp, val.av_val, txn, unpublish);
    } else if(AVMATCH(&method, &av_createStream))
    {
        int i;
        for (i = 0; i < RTMP_MAX_STREAMS; i++) {
            if (!rtmp->streams[i]) {
                rtmp_stream *stream = malloc(sizeof(rtmp_stream));
                if (!stream) { // TODO something drastic
                    fprintf(stderr, "Out of memory for stream!\n");
                    return;
                }
                stream->id = i;
                rtmp->streams[i] = stream;
                break;
            }
        }
        if (i != RTMP_MAX_STREAMS)
            send_result(rtmp, txn, rtmp->streams[i]->id);
        else
            fprintf(stderr, "Maximum number of streams exceeded!\n");
    } else if(AVMATCH(&method, &av_publish))
    {
        AVal type;
        rtmp_stream *stream;
        // transaction id (index 1) is always zero here,
        // command object (index 2) is always null here.
        AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &val);
        AMFProp_GetString(AMF_GetProp(&obj, NULL, 4), &type); //XXX live/recod/append
        stream = rtmp->streams[pkt->msg_id];
        if (!stream) {
            fprintf(stderr, "Unable to publish; stream ID invalid.\n");
            return;
        }
                stream->name = malloc(val.av_len + 1);
                if (!stream->name) { // TODO something drastic
                    free(stream);
                    fprintf(stderr, "Out of memory for stream name!\n");
                    return;
                }
                strncpy(stream->name, val.av_val, val.av_len);
                stream->name[val.av_len] = '\0';

                // use strncmp variant because the type is not likely to
                // be null-terminated, so avoid a 1-byte overread. Note
                // the type is usually the last element in the packet body
                if (!strncmp(type.av_val, "live", 4)) {
                    stream->type = LIVE;
                } else if (!strncmp(type.av_val, "record", 6)) {
                    stream->type = RECORD;
                } else if (!strncmp(type.av_val, "append", 6)) {
                    stream->type = APPEND;
                }
        send_onstatus(rtmp, val.av_val, publish);
        fprintf(stdout, "publishing %s (id %d)\n",
                stream->name, stream->id);
        rtmp->file = fopen(stream->name, "w+b");
        if (0 == rtmp->file) {
            fprintf(stderr, "Failed to open file %s\n", stream->name);
            return;
        }
        {
        int flv01 = MKTAG('F','L','V',0x01);
        char av = 5; //has audio, has video (binary 0b101)
        int len = htonl(9), zero = 0;
        fwrite(&flv01, 4, 1, rtmp->file);
        fwrite(&av, 1, 1, rtmp->file);
        fwrite(&len, 4, 1, rtmp->file);
        fwrite(&zero, 4, 1, rtmp->file); // previous tag size
        }
    } else if(AVMATCH(&method, &av_deleteStream))
    {
        int stream_id;
        stream_id = (int)AMFProp_GetNumber(AMF_GetProp(&obj, NULL, 3));
        if (!rtmp->streams[stream_id]) {
            fprintf(stderr, "Unable to delete stream; invalid id %d\n", stream_id);
            return;
        }
        // TODO only for published streams
        send_onstatus(rtmp, rtmp->streams[stream_id]->name, unpublish);
        rtmp_free_stream(&rtmp->streams[stream_id]);
        fprintf(stderr, "Deleting stream %d\n", stream_id);
    } else if(AVMATCH(&method, &av_play))
    {
        AMFProp_GetString(AMF_GetProp(&obj, NULL, 3), &val);
        send_onstatus(rtmp, val.av_val, play);
        //ctx->stream.fds[ctx->stream.cxn_count++] = rtmp;
    }
    AMF_Reset(&obj);

    return;

invoke_error:
    fprintf(stderr, "%s\n", errstr);

}
