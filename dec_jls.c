/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / JPEG-LS decoder filter, based on CharLS
 *  (https://github.com/team-charls/charls) through its C API.
 *
 *  Samples deeper than 8 bits are shifted down to 8 bits, and planar scans are
 *  interleaved here, so the filter always outputs 8-bit greyscale or RGB.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <charls/charls.h>

/* CharLS reports errors by throwing, and turns the exception back into an
 * error code in its C API. The host solver exports the rest of the C++ ABI
 * this module needs, but not __cxa_throw (checked on both solver_1 and
 * solver_minimal_1), so without this definition the side module has an
 * unresolved import and never instantiates.
 *
 * Consequence to be aware of: a JPEG-LS stream that makes CharLS throw ends
 * the module here instead of coming back as GF_NON_COMPLIANT_BITSTREAM. Well
 * formed streams never reach it. Removing this stub requires __cxa_throw to be
 * exported by the solver.
 */
void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *))
{
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JLSDec] CharLS raised an exception, aborting the module\n"));
	abort();
}

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_JLSDecCtx;

static GF_Err jlsdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_JLSDecCtx *ctx = (GF_JLSDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	return GF_OK;
}

static Bool jlsdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_JLSDecCtx *ctx = (GF_JLSDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err jlsdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output, *decoded = NULL;
	u32 size, out_size, x, y, nb_comp, bytes_per_sample, shift;
	size_t dest_size = 0;
	charls_jpegls_decoder *dec = NULL;
	charls_frame_info info;
	charls_interleave_mode interleave = CHARLS_INTERLEAVE_MODE_NONE;
	GF_JLSDecCtx *ctx = (GF_JLSDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	dec = charls_jpegls_decoder_create();
	if (!dec)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	if (charls_jpegls_decoder_set_source_buffer(dec, data, size) ||
		charls_jpegls_decoder_read_header(dec) ||
		charls_jpegls_decoder_get_frame_info(dec, &info) ||
		charls_jpegls_decoder_get_destination_size(dec, 0, &dest_size))
	{
		charls_jpegls_decoder_destroy(dec);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JLSDec] Not a valid JPEG-LS bitstream\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	charls_jpegls_decoder_get_interleave_mode(dec, 0, &interleave);

	decoded = (u8 *)gf_malloc(dest_size);
	if (!decoded)
	{
		charls_jpegls_decoder_destroy(dec);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	if (charls_jpegls_decoder_decode_to_buffer(dec, decoded, dest_size, 0))
	{
		gf_free(decoded);
		charls_jpegls_decoder_destroy(dec);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JLSDec] Failed to decode JPEG-LS image\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	charls_jpegls_decoder_destroy(dec);
	gf_filter_pid_drop_packet(ctx->ipid);

	nb_comp = info.component_count;
	if ((nb_comp != 1) && (nb_comp != 3))
	{
		gf_free(decoded);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JLSDec] Unsupported component count %d\n", nb_comp));
		return GF_NOT_SUPPORTED;
	}
	bytes_per_sample = (info.bits_per_sample > 8) ? 2 : 1;
	shift = (info.bits_per_sample > 8) ? (info.bits_per_sample - 8) : 0;

	out_size = info.width * info.height * nb_comp;

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(info.width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(info.height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(info.width * nb_comp));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT((nb_comp == 3) ? GF_PIXEL_RGB : GF_PIXEL_GREYSCALE));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck)
	{
		gf_free(decoded);
		return GF_OUT_OF_MEM;
	}

	for (y = 0; y < info.height; y++)
	{
		for (x = 0; x < info.width; x++)
		{
			u32 c;
			for (c = 0; c < nb_comp; c++)
			{
				/* CHARLS_INTERLEAVE_MODE_NONE stores one full plane after
				 * another; the other two modes are already interleaved. */
				size_t src_index = (interleave == CHARLS_INTERLEAVE_MODE_NONE)
									   ? ((size_t)c * info.width * info.height + (size_t)y * info.width + x)
									   : (((size_t)y * info.width + x) * nb_comp + c);
				u32 v;
				if (bytes_per_sample == 2)
				{
					const u16 *s16 = (const u16 *)decoded;
					v = s16[src_index] >> shift;
				}
				else
				{
					v = decoded[src_index];
				}
				output[((size_t)y * info.width + x) * nb_comp + c] = (u8)v;
			}
		}
	}
	gf_free(decoded);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void jlsdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability JLSDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "jls"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/jls|image/x-jls"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister JLSDecoderRegister = {
	.name = "jlsdec",
	GF_FS_SET_DESCRIPTION("JPEG-LS image decoder")
		GF_FS_SET_HELP("This filter decodes JPEG-LS (ISO/IEC 14495-1) images using CharLS.")
			.private_size = sizeof(GF_JLSDecCtx),
	SETCAPS(JLSDecCaps),
	.configure_pid = jlsdec_configure_pid,
	.process = jlsdec_process,
	.process_event = jlsdec_process_event,
	.finalize = jlsdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE jlsdec_register(GF_FilterSession *session)
{
	return &JLSDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_jlsdec(void) {
    gf_filter_auto_register("jlsdec", jlsdec_register);
}
