////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_utils.c

// This module provides the high-level API for creating WavPack files from
// audio data. It manages the buffers used to deinterleave the data passed
// in from the application into the individual streams and it handles the
// generation of riff headers and the "fixup" on the first WavPack block
// header for the case where the number of samples was unknown (or wrong).
// The actual audio stream compression is handled in the pack.c module.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

///////////////////////////// executable code ////////////////////////////////

// Open context for writing WavPack files. The returned context pointer is used
// in all following calls to the library. The "blockout" function will be used
// to store the actual completed WavPack blocks and will be called with the id
// pointers containing user defined data (one for the wv file and one for the
// wvc file). A return value of NULL indicates that memory could not be
// allocated for the context.

WavpackContext *WavpackOpenFileOutput (WavpackBlockOutput blockout, void *wv_id, void *wvc_id)
{
    WavpackContext *wpc = malloc (sizeof (WavpackContext));

    if (!wpc)
        return NULL;

    CLEAR (*wpc);
    wpc->stream_version = CUR_STREAM_VERS;
    wpc->blockout = blockout;
    wpc->wv_out = wv_id;
    wpc->wvc_out = wvc_id;
    return wpc;
}

static int add_to_metadata (WavpackContext *wpc, void *data, uint32_t bcount, unsigned char id);

// New for version 5.0, this function allows the application to store a file extension and a
// file_format identification. The extension would be used by the unpacker if the user had not
// specified the target filename, and specifically handles the case where the original file
// had the "wrong" extension for the file format (e.g., a Wave64 file having a "wav" extension)
// or an alternative (e.g., "bwf") or where the file format is not known. Specifying a file
// format besides the default WP_FORMAT_WAV will ensure that old decoders will not be able to
// see the non-wav wrapper provided with WavpackAddWrapper() (which they would end up putting
// on a file with a .wav extension).

void WavpackSetFileInformation (WavpackContext *wpc, char *file_extension, unsigned char file_format)
{
    if (file_extension && strlen (file_extension) < sizeof (wpc->file_extension)) {
        add_to_metadata (wpc, file_extension, (uint32_t) strlen (file_extension), ID_ALT_EXTENSION);
        strcpy (wpc->file_extension, file_extension);
    }

    wpc->file_format = file_format;
}

// Set configuration for writing WavPack files. This must be done before
// sending any actual samples, however it is okay to send wrapper or other
// metadata before calling this. The "config" structure contains the following
// required information:

// config->bytes_per_sample     see WavpackGetBytesPerSample() for info
// config->bits_per_sample      see WavpackGetBitsPerSample() for info
// config->channel_mask         Microsoft standard (mono = 4, stereo = 3)
// config->num_channels         self evident
// config->sample_rate          self evident

// In addition, the following fields and flags may be set:

// config->flags:
// --------------
// o CONFIG_HYBRID_FLAG         select hybrid mode (must set bitrate)
// o CONFIG_JOINT_STEREO        select joint stereo (must set override also)
// o CONFIG_JOINT_OVERRIDE      override default joint stereo selection
// o CONFIG_HYBRID_SHAPE        select hybrid noise shaping (set override &
//                                                      shaping_weight != 0.0)
// o CONFIG_SHAPE_OVERRIDE      override default hybrid noise shaping
//                               (set CONFIG_HYBRID_SHAPE and shaping_weight)
// o CONFIG_FAST_FLAG           "fast" compression mode
// o CONFIG_HIGH_FLAG           "high" compression mode
// o CONFIG_BITRATE_KBPS        hybrid bitrate is kbps, not bits / sample
// o CONFIG_CREATE_WVC          create correction file
// o CONFIG_OPTIMIZE_WVC        maximize bybrid compression (-cc option)
// o CONFIG_CALC_NOISE          calc noise in hybrid mode
// o CONFIG_EXTRA_MODE          extra processing mode (slow!)
// o CONFIG_SKIP_WVX            no wvx stream for floats & large ints
// o CONFIG_MD5_CHECKSUM        specify if you plan to store MD5 signature
// o CONFIG_CREATE_EXE          specify if you plan to prepend sfx module
// o CONFIG_OPTIMIZE_MONO       detect and optimize for mono files posing as
//                               stereo (uses a more recent stream format that
//                               is not compatible with decoders < 4.3)

// config->bitrate              hybrid bitrate in either bits/sample or kbps
// config->shaping_weight       hybrid noise shaping coefficient override
// config->block_samples        force samples per WavPack block (0 = use deflt)
// config->float_norm_exp       select floating-point data (127 for +/-1.0)
// config->xmode                extra mode processing value override

// If the number of samples to be written is known then it should be passed
// here. If the duration is not known then pass -1. In the case that the size
// is not known (or the writing is terminated early) then it is suggested that
// the application retrieve the first block written and let the library update
// the total samples indication. A function is provided to do this update and
// it should be done to the "correction" file also. If this cannot be done
// (because a pipe is being used, for instance) then a valid WavPack will still
// be created, but when applications want to access that file they will have
// to seek all the way to the end to determine the actual duration. Also, if
// a RIFF header has been included then it should be updated as well or the
// WavPack file will not be directly unpackable to a valid wav file (although
// it will still be usable by itself). A return of FALSE indicates an error.

static const uint32_t stereo_pairings [] = {
    (1 << 0) | (1 << 1),        // FL, FR
    (1 << 4) | (1 << 5),        // BL, BR
    (1 << 6) | (1 << 7),        // FLC, FRC
    (1 << 9) | (1 << 10),       // SL, SR
    (1 << 12) | (1 << 14),      // TFL, TFR
    (1 << 15) | (1 << 17),      // TBL, TBR
    (1 << 29) | (1 << 30)       // stereo mix L,R (RF64)
};

#define NUM_STEREO_PAIRINGS (sizeof (stereo_pairings) / sizeof (stereo_pairings [0]))

int WavpackSetConfiguration (WavpackContext *wpc, WavpackConfig *config, uint32_t total_samples)
{
    return WavpackSetConfiguration64 (wpc, config, total_samples == (uint32_t) -1 ? -1 : total_samples);
}

int WavpackSetConfiguration64 (WavpackContext *wpc, WavpackConfig *config, int64_t total_samples)
{
    uint32_t flags, bps = 0, shift = 0;
    uint32_t chan_mask = config->channel_mask;
    int num_chans = config->num_channels;
    int i;

    if ((config->qmode & QMODE_DSD_AUDIO) && config->bytes_per_sample == 1 && config->bits_per_sample == 8) {
        wpc->dsd_multiplier = 1;
        flags = DSD_FLAG;

        for (i = 14; i >= 0; --i)
            if (config->sample_rate % sample_rates [i] == 0) {
                int divisor = config->sample_rate / sample_rates [i];

                if (divisor && (divisor & (divisor - 1)) == 0) {
                    config->sample_rate /= divisor;
                    wpc->dsd_multiplier = divisor;
                    break;
                }
            }

        // with DSD, very few PCM options work (or make sense), so only allow those that do
        config->flags &= (CONFIG_HIGH_FLAG | CONFIG_MD5_CHECKSUM | CONFIG_PAIR_UNDEF_CHANS);
        config->float_norm_exp = config->xmode = 0;
    }
    else
        flags = config->bytes_per_sample - 1;

    wpc->total_samples = total_samples;
    wpc->config.sample_rate = config->sample_rate;
    wpc->config.num_channels = config->num_channels;
    wpc->config.channel_mask = config->channel_mask;
    wpc->config.bits_per_sample = config->bits_per_sample;
    wpc->config.bytes_per_sample = config->bytes_per_sample;
    wpc->config.block_samples = config->block_samples;
    wpc->config.flags = config->flags;
    wpc->config.qmode = config->qmode;

    if (config->flags & CONFIG_VERY_HIGH_FLAG)
        wpc->config.flags |= CONFIG_HIGH_FLAG;

    for (i = 0; i < 15; ++i)
        if (wpc->config.sample_rate == sample_rates [i])
            break;

    flags |= i << SRATE_LSB;
    flags |= shift << SHIFT_LSB;

    // all of this stuff only applies to PCM

    if (!(flags & DSD_FLAG)) {
        if (config->float_norm_exp) {
            wpc->config.float_norm_exp = config->float_norm_exp;
            wpc->config.flags |= CONFIG_FLOAT_DATA;
            flags |= FLOAT_DATA;
        }
        else
            shift = (config->bytes_per_sample * 8) - config->bits_per_sample;

        if (config->flags & CONFIG_HYBRID_FLAG) {
            flags |= HYBRID_FLAG | HYBRID_BITRATE | HYBRID_BALANCE;

            if (!(wpc->config.flags & CONFIG_SHAPE_OVERRIDE)) {
                wpc->config.flags |= CONFIG_HYBRID_SHAPE | CONFIG_AUTO_SHAPING;
                flags |= HYBRID_SHAPE | NEW_SHAPING;
            }
            else if (wpc->config.flags & CONFIG_HYBRID_SHAPE) {
                wpc->config.shaping_weight = config->shaping_weight;
                flags |= HYBRID_SHAPE | NEW_SHAPING;
            }

            if (wpc->config.flags & (CONFIG_CROSS_DECORR | CONFIG_OPTIMIZE_WVC))
                flags |= CROSS_DECORR;

            if (config->flags & CONFIG_BITRATE_KBPS) {
                bps = (uint32_t) floor (config->bitrate * 256000.0 / config->sample_rate / config->num_channels + 0.5);

                if (bps > (64 << 8))
                    bps = 64 << 8;
            }
            else
                bps = (uint32_t) floor (config->bitrate * 256.0 + 0.5);
        }
        else
            flags |= CROSS_DECORR;

        if (!(config->flags & CONFIG_JOINT_OVERRIDE) || (config->flags & CONFIG_JOINT_STEREO))
            flags |= JOINT_STEREO;

        if (config->flags & CONFIG_CREATE_WVC)
            wpc->wvc_flag = TRUE;
    }

    for (wpc->current_stream = 0; num_chans; wpc->current_stream++) {
        WavpackStream *wps = malloc (sizeof (WavpackStream));
        uint32_t stereo_mask = 0, mono_mask = 0;
        int pos, chans = 0;

        wpc->streams = realloc (wpc->streams, (wpc->current_stream + 1) * sizeof (wpc->streams [0]));
        wpc->streams [wpc->current_stream] = wps;
        CLEAR (*wps);

        for (pos = 0; pos < 32; ++pos)
            if (chan_mask & (1 << pos)) {
                if (mono_mask) {
                    stereo_mask = mono_mask | (1 << pos);
                    break;
                }
                else
                    mono_mask = 1 << pos;
            }

        if (num_chans > 1 && stereo_mask) {
            for (i = 0; i < NUM_STEREO_PAIRINGS; ++i)
                if (stereo_mask == stereo_pairings [i]) {
                    chan_mask &= ~stereo_mask;
                    chans = 2;
                    break;
                }

            if (i == NUM_STEREO_PAIRINGS) {
                chan_mask &= ~mono_mask;
                chans = 1;
            }
        }
        else if (mono_mask) {
            chan_mask &= ~mono_mask;
            chans = 1;
        }

        if (!chans) {
            if (config->flags & CONFIG_PAIR_UNDEF_CHANS)
                chans = num_chans > 1 ? 2 : 1;
            else
                chans = 1;
        }

        num_chans -= chans;

        if (num_chans && wpc->current_stream == NEW_MAX_STREAMS - 1)
            break;

        memcpy (wps->wphdr.ckID, "wvpk", 4);
        wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
        SET_TOTAL_SAMPLES (wps->wphdr, wpc->total_samples);
        wps->wphdr.version = wpc->stream_version;
        wps->wphdr.flags = flags;
        wps->bits = bps;

        if (!wpc->current_stream)
            wps->wphdr.flags |= INITIAL_BLOCK;

        if (!num_chans)
            wps->wphdr.flags |= FINAL_BLOCK;

        if (chans == 1) {
            wps->wphdr.flags &= ~(JOINT_STEREO | CROSS_DECORR | HYBRID_BALANCE);
            wps->wphdr.flags |= MONO_FLAG;
        }
    }

    wpc->num_streams = wpc->current_stream;
    wpc->current_stream = 0;

    if (num_chans) {
        strcpy (wpc->error_message, "too many channels!");
        return FALSE;
    }

    if (config->flags & CONFIG_EXTRA_MODE)
        wpc->config.xmode = config->xmode ? config->xmode : 1;

    return TRUE;
}

// This function allows setting the Core Audio File channel layout, many of which do not
// conform to the Microsoft ordering standard that Wavpack requires internally (at least for
// those channels present in the "channel mask"). In addition to the layout tag, this function
// allows a reordering string to be stored in the file to allow the unpacker to reorder the
// channels back to the specified layout (if it is aware of this feature and wants to restore
// the CAF order). The number of channels in the layout is specified in the lower nybble of
// the layout word, and if a reorder string is specified it must be that long. Note that all
// the reordering is actually done outside of this library, and that if reordering is done
// then the appropriate qmode bit must be set to ensure that any MD5 sum is stored with a new
// ID so that old decoders don't try to verify it (and to let the decoder know that a reorder
// might be required).

int WavpackSetChannelLayout (WavpackContext *wpc, uint32_t layout_tag, const unsigned char *reorder)
{
    int nchans = layout_tag & 0xff;

    if ((layout_tag & 0xff00ff00) || nchans > wpc->config.num_channels)
        return FALSE;

    wpc->channel_layout = layout_tag;

    if (wpc->channel_reordering) {
        free (wpc->channel_reordering);
        wpc->channel_reordering = NULL;
    }

    if (nchans && reorder) {
        int min_index = 256, i;

        for (i = 0; i < nchans; ++i)
            if (reorder [i] < min_index)
                min_index = reorder [i];

        wpc->channel_reordering = malloc (nchans);

        if (wpc->channel_reordering)
            for (i = 0; i < nchans; ++i)
                wpc->channel_reordering [i] = reorder [i] - min_index;
    }

    return TRUE;
}

// Prepare to actually pack samples by determining the size of the WavPack
// blocks and allocating sample buffers and initializing each stream. Call
// after WavpackSetConfiguration() and before WavpackPackSamples(). A return
// of FALSE indicates an error.

static int write_metadata_block (WavpackContext *wpc);

int WavpackPackInit (WavpackContext *wpc)
{
    if (wpc->metabytes > 16384)             // 16384 bytes still leaves plenty of room for audio
        write_metadata_block (wpc);         //  in this block (otherwise write a special one)

    if (wpc->config.flags & CONFIG_HIGH_FLAG)
        wpc->block_samples = wpc->config.sample_rate;
    else if (!(wpc->config.sample_rate % 2))
        wpc->block_samples = wpc->config.sample_rate / 2;
    else
        wpc->block_samples = wpc->config.sample_rate;

    if (wpc->dsd_multiplier) {
        if (wpc->config.flags & CONFIG_HIGH_FLAG)
            wpc->block_samples = 22050;
        else
            wpc->block_samples = 44100;

        if (wpc->config.num_channels == 1)
            wpc->block_samples *= 2;
    }

    while (wpc->block_samples * wpc->config.num_channels > 150000)
        wpc->block_samples /= 2;

    while (wpc->block_samples * wpc->config.num_channels < 40000)
        wpc->block_samples *= 2;

    if (wpc->config.block_samples) {
        if ((wpc->config.flags & CONFIG_MERGE_BLOCKS) &&
            wpc->block_samples > (uint32_t) wpc->config.block_samples) {
                wpc->block_boundary = wpc->config.block_samples;
                wpc->block_samples /= wpc->config.block_samples;
                wpc->block_samples *= wpc->config.block_samples;
        }
        else
            wpc->block_samples = wpc->config.block_samples;
    }

    wpc->ave_block_samples = wpc->block_samples;
    wpc->max_samples = wpc->block_samples + (wpc->block_samples >> 1);

    for (wpc->current_stream = 0; wpc->current_stream < wpc->num_streams; wpc->current_stream++) {
        WavpackStream *wps = wpc->streams [wpc->current_stream];

        wps->sample_buffer = malloc (wpc->max_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));

        if (wps->wphdr.flags & DSD_FLAG)
            pack_dsd_init (wpc);
        else
            pack_init (wpc);
    }

    return TRUE;
}

// Pack the specified samples. Samples must be stored in longs in the native
// endian format of the executing processor. The number of samples specified
// indicates composite samples (sometimes called "frames"). So, the actual
// number of data points would be this "sample_count" times the number of
// channels. Note that samples are accumulated here until enough exist to
// create a complete WavPack block (or several blocks for multichannel audio).
// If an application wants to break a block at a specific sample, then it must
// simply call WavpackFlushSamples() to force an early termination. Completed
// WavPack blocks are send to the function provided in the initial call to
// WavpackOpenFileOutput(). A return of FALSE indicates an error.

static int pack_streams (WavpackContext *wpc, uint32_t block_samples);
static int create_riff_header (WavpackContext *wpc, int64_t total_samples, void *outbuffer);
static int pack_dsd_samples (WavpackContext *wpc, unsigned char *sample_buffer, uint32_t sample_count);

int WavpackPackSamples (WavpackContext *wpc, int32_t *sample_buffer, uint32_t sample_count)
{
    int nch = wpc->config.num_channels;

    while (sample_count) {
        int32_t *source_pointer = sample_buffer;
        unsigned int samples_to_copy;

        if (!wpc->riff_header_added && !wpc->riff_header_created && !wpc->file_format) {
            char riff_header [128];

            if (!add_to_metadata (wpc, riff_header, create_riff_header (wpc, wpc->total_samples, riff_header), ID_RIFF_HEADER))
                return FALSE;
        }

        if (wpc->acc_samples + sample_count > wpc->max_samples)
            samples_to_copy = wpc->max_samples - wpc->acc_samples;
        else
            samples_to_copy = sample_count;

        for (wpc->current_stream = 0; wpc->current_stream < wpc->num_streams; wpc->current_stream++) {
            WavpackStream *wps = wpc->streams [wpc->current_stream];
            int32_t *dptr, *sptr, cnt;

            dptr = wps->sample_buffer + wpc->acc_samples * (wps->wphdr.flags & MONO_FLAG ? 1 : 2);
            sptr = source_pointer;
            cnt = samples_to_copy;

            // This code used to just copy the 32-bit samples regardless of the actual size with the
            // assumption that the caller had properly sign-extended the values (if they were smaller
            // than 32 bits). However, several people have discovered that if the data isn't properly
            // sign extended then ugly things happen (e.g. CRC errors that show up only on decode).
            // To prevent this, we now explicitly sign-extend samples smaller than 32-bit when we
            // copy, and the performance hit from doing this is very small (generally < 1%).

            if (wps->wphdr.flags & MONO_FLAG) {
                switch (wpc->config.bytes_per_sample) {
                    case 1:
                        while (cnt--) {
                            *dptr++ = (signed char) *sptr;
                            sptr += nch;
                        }

                        break;

                    case 2:
                        while (cnt--) {
                            *dptr++ = (int16_t) *sptr;
                            sptr += nch;
                        }

                        break;

                    case 3:
                        while (cnt--) {
                            *dptr++ = (*sptr << 8) >> 8;
                            sptr += nch;
                        }

                        break;

                    default:
                        while (cnt--) {
                            *dptr++ = *sptr;
                            sptr += nch;
                        }
                }

                source_pointer++;
            }
            else {
                switch (wpc->config.bytes_per_sample) {
                    case 1:
                        while (cnt--) {
                            *dptr++ = (signed char) sptr [0];
                            *dptr++ = (signed char) sptr [1];
                            sptr += nch;
                        }

                        break;

                    case 2:
                        while (cnt--) {
                            *dptr++ = (int16_t) sptr [0];
                            *dptr++ = (int16_t) sptr [1];
                            sptr += nch;
                        }

                        break;

                    case 3:
                        while (cnt--) {
                            *dptr++ = (sptr [0] << 8) >> 8;
                            *dptr++ = (sptr [1] << 8) >> 8;
                            sptr += nch;
                        }

                        break;

                    default:
                        while (cnt--) {
                            *dptr++ = sptr [0];
                            *dptr++ = sptr [1];
                            sptr += nch;
                        }
                }

                source_pointer += 2;
            }
        }

        sample_buffer += samples_to_copy * nch;
        sample_count -= samples_to_copy;

        if ((wpc->acc_samples += samples_to_copy) == wpc->max_samples &&
            !pack_streams (wpc, wpc->block_samples))
                return FALSE;
    }

    return TRUE;
}

// Flush all accumulated samples into WavPack blocks. This is normally called
// after all samples have been sent to WavpackPackSamples(), but can also be
// called to terminate a WavPack block at a specific sample (in other words it
// is possible to continue after this operation). This is also called to
// dump non-audio blocks like those holding metadata for various purposes.
// A return of FALSE indicates an error.

int WavpackFlushSamples (WavpackContext *wpc)
{
    while (wpc->acc_samples) {
        uint32_t block_samples;

        if (wpc->acc_samples > wpc->block_samples)
            block_samples = wpc->acc_samples / 2;
        else
            block_samples = wpc->acc_samples;

        if (!pack_streams (wpc, block_samples))
            return FALSE;
    }

    if (wpc->metacount)
        write_metadata_block (wpc);

    return TRUE;
}

// Note: The following function is no longer required because a proper wav
// header is now automatically generated for the application. However, if the
// application wants to generate its own header or wants to include additional
// chunks, then this function can still be used in which case the automatic
// wav header generation is suppressed.

// Add wrapper (currently RIFF only) to WavPack blocks. This should be called
// before sending any audio samples for the RIFF header or after all samples
// have been sent for any RIFF trailer. WavpackFlushSamples() should be called
// between sending the last samples and calling this for trailer data to make
// sure that headers and trailers don't get mixed up in very short files. If
// the exact contents of the RIFF header are not known because, for example,
// the file duration is uncertain or trailing chunks are possible, simply write
// a "dummy" header of the correct length. When all data has been written it
// will be possible to read the first block written and update the header
// directly. An example of this can be found in the Audition filter. A
// return of FALSE indicates an error.

int WavpackAddWrapper (WavpackContext *wpc, void *data, uint32_t bcount)
{
    int64_t index = WavpackGetSampleIndex64 (wpc);
    unsigned char meta_id;

    if (!index || index == -1) {
        wpc->riff_header_added = TRUE;
        meta_id = wpc->file_format ? ID_ALT_HEADER : ID_RIFF_HEADER;
    }
    else {
        wpc->riff_trailer_bytes += bcount;
        meta_id = wpc->file_format ? ID_ALT_TRAILER : ID_RIFF_TRAILER;
    }

    return add_to_metadata (wpc, data, bcount, meta_id);
}

// Store computed MD5 sum in WavPack metadata. Note that the user must compute
// the 16 byte sum; it is not done here. A return of FALSE indicates an error.
// If any of the lower 8 bits of qmode are set, then this MD5 is stored with
// a metadata ID that old decoders do not recognize (because they would not
// interpret the qmode and would therefore fail the verification).

int WavpackStoreMD5Sum (WavpackContext *wpc, unsigned char data [16])
{
    return add_to_metadata (wpc, data, 16, (wpc->config.qmode & 0xff) ? ID_ALT_MD5_CHECKSUM : ID_MD5_CHECKSUM);
}

#pragma pack(push,4)

typedef struct {
    char ckID [4];
    uint64_t chunkSize64;
} CS64Chunk;

typedef struct {
    uint64_t riffSize64, dataSize64, sampleCount64;
    uint32_t tableLength;
} DS64Chunk;

typedef struct {
    char ckID [4];
    uint32_t ckSize;
    char junk [28];
} JunkChunk;

#pragma pack(pop)

#define DS64ChunkFormat "DDDL"

static int create_riff_header (WavpackContext *wpc, int64_t total_samples, void *outbuffer)
{
    int do_rf64 = 0, write_junk = 1;
    ChunkHeader ds64hdr, datahdr, fmthdr;
    char *outptr = outbuffer;
    RiffChunkHeader riffhdr;
    DS64Chunk ds64_chunk;
    JunkChunk junkchunk;
    WaveHeader wavhdr;

    int64_t total_data_bytes, total_riff_bytes;
    int32_t channel_mask = wpc->config.channel_mask;
    int32_t sample_rate = wpc->config.sample_rate;
    int bytes_per_sample = wpc->config.bytes_per_sample;
    int bits_per_sample = wpc->config.bits_per_sample;
    int format = (wpc->config.float_norm_exp) ? 3 : 1;
    int num_channels = wpc->config.num_channels;
    int wavhdrsize = 16;

    wpc->riff_header_created = TRUE;

    if (format == 3 && wpc->config.float_norm_exp != 127) {
        strcpy (wpc->error_message, "can't create valid RIFF wav header for non-normalized floating data!");
        return FALSE;
    }

    if (total_samples == -1)
        total_samples = 0x7ffff000 / (bytes_per_sample * num_channels);

    total_data_bytes = total_samples * bytes_per_sample * num_channels;

    if (total_data_bytes > 0xff000000) {
        write_junk = 0;
        do_rf64 = 1;
    }

    CLEAR (wavhdr);

    wavhdr.FormatTag = format;
    wavhdr.NumChannels = num_channels;
    wavhdr.SampleRate = sample_rate;
    wavhdr.BytesPerSecond = sample_rate * num_channels * bytes_per_sample;
    wavhdr.BlockAlign = bytes_per_sample * num_channels;
    wavhdr.BitsPerSample = bits_per_sample;

    if (num_channels > 2 || channel_mask != 0x5 - num_channels) {
        wavhdrsize = sizeof (wavhdr);
        wavhdr.cbSize = 22;
        wavhdr.ValidBitsPerSample = bits_per_sample;
        wavhdr.SubFormat = format;
        wavhdr.ChannelMask = channel_mask;
        wavhdr.FormatTag = 0xfffe;
        wavhdr.BitsPerSample = bytes_per_sample * 8;
        wavhdr.GUID [4] = 0x10;
        wavhdr.GUID [6] = 0x80;
        wavhdr.GUID [9] = 0xaa;
        wavhdr.GUID [11] = 0x38;
        wavhdr.GUID [12] = 0x9b;
        wavhdr.GUID [13] = 0x71;
    }

    strncpy (riffhdr.ckID, do_rf64 ? "RF64" : "RIFF", sizeof (riffhdr.ckID));
    strncpy (riffhdr.formType, "WAVE", sizeof (riffhdr.formType));
    total_riff_bytes = sizeof (riffhdr) + wavhdrsize + sizeof (datahdr) + total_data_bytes + wpc->riff_trailer_bytes;
    if (do_rf64) total_riff_bytes += sizeof (ds64hdr) + sizeof (ds64_chunk);
    if (write_junk) total_riff_bytes += sizeof (junkchunk);
    strncpy (fmthdr.ckID, "fmt ", sizeof (fmthdr.ckID));
    strncpy (datahdr.ckID, "data", sizeof (datahdr.ckID));
    fmthdr.ckSize = wavhdrsize;

    if (write_junk) {
        CLEAR (junkchunk);
        strncpy (junkchunk.ckID, "junk", sizeof (junkchunk.ckID));
        junkchunk.ckSize = sizeof (junkchunk) - 8;
        WavpackNativeToLittleEndian (&junkchunk, ChunkHeaderFormat);
    }

    if (do_rf64) {
        strncpy (ds64hdr.ckID, "ds64", sizeof (ds64hdr.ckID));
        ds64hdr.ckSize = sizeof (ds64_chunk);
        CLEAR (ds64_chunk);
        ds64_chunk.riffSize64 = total_riff_bytes;
        ds64_chunk.dataSize64 = total_data_bytes;
        ds64_chunk.sampleCount64 = total_samples;
        riffhdr.ckSize = (uint32_t) -1;
        datahdr.ckSize = (uint32_t) -1;
        WavpackNativeToLittleEndian (&ds64hdr, ChunkHeaderFormat);
        WavpackNativeToLittleEndian (&ds64_chunk, DS64ChunkFormat);
    }
    else {
        riffhdr.ckSize = (uint32_t) total_riff_bytes;
        datahdr.ckSize = (uint32_t) total_data_bytes;
    }

    WavpackNativeToLittleEndian (&riffhdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&fmthdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&wavhdr, WaveHeaderFormat);
    WavpackNativeToLittleEndian (&datahdr, ChunkHeaderFormat);

    // write the RIFF chunks up to just before the data starts

    outptr = (char *) memcpy (outptr, &riffhdr, sizeof (riffhdr)) + sizeof (riffhdr);

    if (do_rf64) {
        outptr = (char *) memcpy (outptr, &ds64hdr, sizeof (ds64hdr)) + sizeof (ds64hdr);
        outptr = (char *) memcpy (outptr, &ds64_chunk, sizeof (ds64_chunk)) + sizeof (ds64_chunk);
    }

    if (write_junk)
        outptr = (char *) memcpy (outptr, &junkchunk, sizeof (junkchunk)) + sizeof (junkchunk);

    outptr = (char *) memcpy (outptr, &fmthdr, sizeof (fmthdr)) + sizeof (fmthdr);
    outptr = (char *) memcpy (outptr, &wavhdr, wavhdrsize) + wavhdrsize;
    outptr = (char *) memcpy (outptr, &datahdr, sizeof (datahdr)) + sizeof (datahdr);

    return (int)(outptr - (char *) outbuffer);
}

static int pack_streams (WavpackContext *wpc, uint32_t block_samples)
{
    uint32_t max_blocksize, bcount;
    unsigned char *outbuff, *outend, *out2buff, *out2end;
    int result = TRUE;

    if ((wpc->config.flags & CONFIG_FLOAT_DATA) && !(wpc->config.flags & CONFIG_SKIP_WVX))
        max_blocksize = block_samples * 16 + 4096;
    else
        max_blocksize = block_samples * 10 + 4096;

    out2buff = (wpc->wvc_flag) ? malloc (max_blocksize) : NULL;
    out2end = out2buff + max_blocksize;
    outbuff = malloc (max_blocksize);
    outend = outbuff + max_blocksize;

    for (wpc->current_stream = 0; wpc->current_stream < wpc->num_streams; wpc->current_stream++) {
        WavpackStream *wps = wpc->streams [wpc->current_stream];
        uint32_t flags = wps->wphdr.flags;

        flags &= ~MAG_MASK;
        flags += (1 << MAG_LSB) * ((flags & BYTES_STORED) * 8 + 7);

        SET_BLOCK_INDEX (wps->wphdr, wps->sample_index);
        wps->wphdr.block_samples = block_samples;
        wps->wphdr.flags = flags;
        wps->block2buff = out2buff;
        wps->block2end = out2end;
        wps->blockbuff = outbuff;
        wps->blockend = outend;

        if (flags & DSD_FLAG)
            result = pack_dsd_block (wpc, wps->sample_buffer);
        else
            result = pack_block (wpc, wps->sample_buffer);

        wps->blockbuff = wps->block2buff = NULL;

        if (wps->wphdr.block_samples != block_samples)
            block_samples = wps->wphdr.block_samples;

        if (!result) {
            strcpy (wpc->error_message, "output buffer overflowed!");
            break;
        }

        bcount = ((WavpackHeader *) outbuff)->ckSize + 8;
        WavpackNativeToLittleEndian ((WavpackHeader *) outbuff, WavpackHeaderFormat);
        result = wpc->blockout (wpc->wv_out, outbuff, bcount);

        if (!result) {
            strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");
            break;
        }

        wpc->filelen += bcount;

        if (out2buff) {
            bcount = ((WavpackHeader *) out2buff)->ckSize + 8;
            WavpackNativeToLittleEndian ((WavpackHeader *) out2buff, WavpackHeaderFormat);
            result = wpc->blockout (wpc->wvc_out, out2buff, bcount);

            if (!result) {
                strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");
                break;
            }

            wpc->file2len += bcount;
        }

        if (wpc->acc_samples != block_samples)
            memmove (wps->sample_buffer, wps->sample_buffer + block_samples * (flags & MONO_FLAG ? 1 : 2),
                (wpc->acc_samples - block_samples) * sizeof (int32_t) * (flags & MONO_FLAG ? 1 : 2));
    }

    wpc->current_stream = 0;
    wpc->ave_block_samples = (wpc->ave_block_samples * 0x7 + block_samples + 0x4) >> 3;
    wpc->acc_samples -= block_samples;
    free (outbuff);

    if (out2buff)
        free (out2buff);

    return result;
}

// Given the pointer to the first block written (to either a .wv or .wvc file),
// update the block with the actual number of samples written. If the wav
// header was generated by the library, then it is updated also. This should
// be done if WavpackSetConfiguration() was called with an incorrect number
// of samples (or -1). It is the responsibility of the application to read and
// rewrite the block. An example of this can be found in the Audition filter.

void WavpackUpdateNumSamples (WavpackContext *wpc, void *first_block)
{
    uint32_t wrapper_size;

    WavpackLittleEndianToNative (first_block, WavpackHeaderFormat);
    SET_TOTAL_SAMPLES (* (WavpackHeader *) first_block, WavpackGetSampleIndex64 (wpc));

    if (wpc->riff_header_created && WavpackGetWrapperLocation (first_block, &wrapper_size)) {
        unsigned char riff_header [128];

        if (wrapper_size == create_riff_header (wpc, WavpackGetSampleIndex64 (wpc), riff_header))
            memcpy (WavpackGetWrapperLocation (first_block, NULL), riff_header, wrapper_size);
    }

    WavpackNativeToLittleEndian (first_block, WavpackHeaderFormat);
}

// Note: The following function is no longer required because the wav header
// automatically generated for the application will also be updated by
// WavpackUpdateNumSamples (). However, if the application wants to generate
// its own header or wants to include additional chunks, then this function
// still must be used to update the application generated header.

// Given the pointer to the first block written to a WavPack file, this
// function returns the location of the stored RIFF header that was originally
// written with WavpackAddWrapper(). This would normally be used to update
// the wav header to indicate that a different number of samples was actually
// written or if additional RIFF chunks are written at the end of the file.
// The "size" parameter can be set to non-NULL to obtain the exact size of the
// RIFF header, and the function will return FALSE if the header is not found
// in the block's metadata (or it is not a valid WavPack block). It is the
// responsibility of the application to read and rewrite the block. An example
// of this can be found in the Audition filter.

static void *find_metadata (void *wavpack_block, int desired_id, uint32_t *size);

void *WavpackGetWrapperLocation (void *first_block, uint32_t *size)
{
    void *loc;

    WavpackLittleEndianToNative (first_block, WavpackHeaderFormat);
    loc = find_metadata (first_block, ID_RIFF_HEADER, size);

    if (!loc)
        loc = find_metadata (first_block, ID_ALT_HEADER, size);

    WavpackNativeToLittleEndian (first_block, WavpackHeaderFormat);

    return loc;
}

static void *find_metadata (void *wavpack_block, int desired_id, uint32_t *size)
{
    WavpackHeader *wphdr = wavpack_block;
    unsigned char *dp, meta_id, c1, c2;
    int32_t bcount, meta_bc;

    if (strncmp (wphdr->ckID, "wvpk", 4))
        return NULL;

    bcount = wphdr->ckSize - sizeof (WavpackHeader) + 8;
    dp = (unsigned char *)(wphdr + 1);

    while (bcount >= 2) {
        meta_id = *dp++;
        c1 = *dp++;

        meta_bc = c1 << 1;
        bcount -= 2;

        if (meta_id & ID_LARGE) {
            if (bcount < 2)
                break;

            c1 = *dp++;
            c2 = *dp++;
            meta_bc += ((uint32_t) c1 << 9) + ((uint32_t) c2 << 17);
            bcount -= 2;
        }

        if ((meta_id & ID_UNIQUE) == desired_id) {
            if ((bcount - meta_bc) >= 0) {
                if (size)
                    *size = meta_bc - ((meta_id & ID_ODD_SIZE) ? 1 : 0);

                return dp;
            }
            else
                return NULL;
        }

        bcount -= meta_bc;
        dp += meta_bc;
    }

    return NULL;
}

int copy_metadata (WavpackMetadata *wpmd, unsigned char *buffer_start, unsigned char *buffer_end)
{
    uint32_t mdsize = wpmd->byte_length + (wpmd->byte_length & 1);
    WavpackHeader *wphdr = (WavpackHeader *) buffer_start;

    mdsize += (wpmd->byte_length > 510) ? 4 : 2;
    buffer_start += wphdr->ckSize + 8;

    if (buffer_start + mdsize >= buffer_end)
        return FALSE;

    buffer_start [0] = wpmd->id | (wpmd->byte_length & 1 ? ID_ODD_SIZE : 0);
    buffer_start [1] = (wpmd->byte_length + 1) >> 1;

    if (wpmd->byte_length > 510) {
        buffer_start [0] |= ID_LARGE;
        buffer_start [2] = (wpmd->byte_length + 1) >> 9;
        buffer_start [3] = (wpmd->byte_length + 1) >> 17;
    }

    if (wpmd->data && wpmd->byte_length) {
        memcpy (buffer_start + (wpmd->byte_length > 510 ? 4 : 2), wpmd->data, wpmd->byte_length);

        if (wpmd->byte_length & 1)          // if size is odd, make sure pad byte is a zero
            buffer_start [mdsize - 1] = 0;
    }

    wphdr->ckSize += mdsize;
    return TRUE;
}

static int add_to_metadata (WavpackContext *wpc, void *data, uint32_t bcount, unsigned char id)
{
    WavpackMetadata *mdp;
    unsigned char *src = data;

    while (bcount) {
        if (wpc->metacount) {
            uint32_t bc = bcount;

            mdp = wpc->metadata + wpc->metacount - 1;

            if (mdp->id == id) {
                if (wpc->metabytes + bcount > 1000000)
                    bc = 1000000 - wpc->metabytes;

                mdp->data = realloc (mdp->data, mdp->byte_length + bc);
                memcpy ((char *) mdp->data + mdp->byte_length, src, bc);
                mdp->byte_length += bc;
                wpc->metabytes += bc;
                bcount -= bc;
                src += bc;

                if (wpc->metabytes >= 1000000 && !write_metadata_block (wpc))
                    return FALSE;
            }
        }

        if (bcount) {
            wpc->metadata = realloc (wpc->metadata, (wpc->metacount + 1) * sizeof (WavpackMetadata));
            mdp = wpc->metadata + wpc->metacount++;
            mdp->byte_length = 0;
            mdp->data = NULL;
            mdp->id = id;
        }
    }

    return TRUE;
}

static char *write_metadata (WavpackMetadata *wpmd, char *outdata)
{
    unsigned char id = wpmd->id, wordlen [3];

    wordlen [0] = (wpmd->byte_length + 1) >> 1;
    wordlen [1] = (wpmd->byte_length + 1) >> 9;
    wordlen [2] = (wpmd->byte_length + 1) >> 17;

    if (wpmd->byte_length & 1)
        id |= ID_ODD_SIZE;

    if (wordlen [1] || wordlen [2])
        id |= ID_LARGE;

    *outdata++ = id;
    *outdata++ = wordlen [0];

    if (id & ID_LARGE) {
        *outdata++ = wordlen [1];
        *outdata++ = wordlen [2];
    }

    if (wpmd->data && wpmd->byte_length) {
        memcpy (outdata, wpmd->data, wpmd->byte_length);
        outdata += wpmd->byte_length;

        if (wpmd->byte_length & 1)
            *outdata++ = 0;
    }

    return outdata;
}

static int write_metadata_block (WavpackContext *wpc)
{
    char *block_buff, *block_ptr;
    WavpackHeader *wphdr;

    if (wpc->metacount) {
        int metacount = wpc->metacount, block_size = sizeof (WavpackHeader);
        WavpackMetadata *wpmdp = wpc->metadata;

        while (metacount--) {
            block_size += wpmdp->byte_length + (wpmdp->byte_length & 1);
            block_size += (wpmdp->byte_length > 510) ? 4 : 2;
            wpmdp++;
        }

        wphdr = (WavpackHeader *) (block_buff = malloc (block_size));

        CLEAR (*wphdr);
        memcpy (wphdr->ckID, "wvpk", 4);
        SET_TOTAL_SAMPLES (*wphdr, wpc->total_samples);
        wphdr->version = wpc->stream_version;
        wphdr->ckSize = block_size - 8;
        wphdr->block_samples = 0;

        block_ptr = (char *)(wphdr + 1);

        wpmdp = wpc->metadata;

        while (wpc->metacount) {
            block_ptr = write_metadata (wpmdp, block_ptr);
            wpc->metabytes -= wpmdp->byte_length;
            free_metadata (wpmdp++);
            wpc->metacount--;
        }

        free (wpc->metadata);
        wpc->metadata = NULL;
        WavpackNativeToLittleEndian ((WavpackHeader *) block_buff, WavpackHeaderFormat);

        if (!wpc->blockout (wpc->wv_out, block_buff, block_size)) {
            free (block_buff);
            strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");
            return FALSE;
        }

        free (block_buff);
    }

    return TRUE;
}

void free_metadata (WavpackMetadata *wpmd)
{
    if (wpmd->data) {
        free (wpmd->data);
        wpmd->data = NULL;
    }
}
