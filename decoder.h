#ifndef FFPLAY_DECODER_H
#define FFPLAY_DECODER_H

#include <SDL.h>
#include <SDL_thread.h>

#include <libavcodec/avcodec.h>

typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue  FrameQueue;

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
    int decoder_reorder_pts;
} Decoder;

int  decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond, int drp);
void decoder_destroy(Decoder *d);
int  decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
int  decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg);
void decoder_abort(Decoder *d, FrameQueue *fq);

#endif // FFPLAY_DECODER_H