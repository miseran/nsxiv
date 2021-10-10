/* Copyright 2011, 2012 Bert Muennich
 *
 * This file is a part of nsxiv.
 *
 * nsxiv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * nsxiv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nsxiv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nsxiv.h"
#define _IMAGE_CONFIG
#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if HAVE_LIBEXIF
#include <libexif/exif-data.h>
#endif

#if HAVE_LIBGIF
#include <gif_lib.h>
enum { DEF_GIF_DELAY = 75 };
#endif

#if HAVE_LIBWEBP
#include <webp/decode.h>
#include <webp/demux.h>
enum { DEF_WEBP_DELAY = 75 };
#endif

void img_init(img_t *img, win_t *win)
{
	imlib_context_set_display(win->env.dpy);
	imlib_context_set_visual(win->env.vis);
	imlib_context_set_colormap(win->env.cmap);

	img->im = NULL;
	img->win = win;
	img->scalemode = options->scalemode;
	img->zoom = options->zoom;
	img->zoom = MAX(img->zoom, ZOOM_MIN);
	img->zoom = MIN(img->zoom, ZOOM_MAX);
	img->checkpan = false;
	img->dirty = false;
	img->aa = ANTI_ALIAS;
	img->alpha = ALPHA_LAYER;
	img->multi.cap = img->multi.cnt = 0;
	img->multi.animate = options->animate;
	img->multi.framedelay = options->framerate > 0 ? 1000 / options->framerate : 0;
	img->multi.length = 0;

	img->cmod = imlib_create_color_modifier();
	imlib_context_set_color_modifier(img->cmod);
	img_change_gamma(img, options->gamma);

	img->ss.on = options->slideshow > 0;
	img->ss.delay = options->slideshow > 0 ? options->slideshow : SLIDESHOW_DELAY * 10;
}

#if HAVE_LIBEXIF
void exif_auto_orientate(const fileinfo_t *file)
{
	ExifData *ed;
	ExifEntry *entry;
	int byte_order, orientation = 0;

	if ((ed = exif_data_new_from_file(file->path)) == NULL)
		return;
	byte_order = exif_data_get_byte_order(ed);
	entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
	if (entry != NULL)
		orientation = exif_get_short(entry->data, byte_order);
	exif_data_unref(ed);

	switch (orientation) {
		case 5:
			imlib_image_orientate(1);
		case 2:
			imlib_image_flip_vertical();
			break;
		case 3:
			imlib_image_orientate(2);
			break;
		case 7:
			imlib_image_orientate(1);
		case 4:
			imlib_image_flip_horizontal();
			break;
		case 6:
			imlib_image_orientate(1);
			break;
		case 8:
			imlib_image_orientate(3);
			break;
	}
}
#endif

#if HAVE_LIBGIF
bool img_load_gif(img_t *img, const fileinfo_t *file)
{
	GifFileType *gif;
	GifRowType *rows = NULL;
	GifRecordType rec;
	ColorMapObject *cmap;
	DATA32 bgpixel, *data, *ptr;
	DATA32 *prev_frame = NULL;
	Imlib_Image im;
	int i, j, bg, r, g, b;
	int x, y, w, h, sw, sh;
	int px, py, pw, ph;
	int intoffset[] = { 0, 4, 2, 1 };
	int intjump[] = { 8, 8, 4, 2 };
	int transp = -1;
	unsigned int disposal = 0, prev_disposal = 0;
	unsigned int delay = 0;
	bool err = false;

	if (img->multi.cap == 0) {
		img->multi.cap = 8;
		img->multi.frames = (img_frame_t*)
		                    emalloc(sizeof(img_frame_t) * img->multi.cap);
	}
	img->multi.cnt = img->multi.sel = 0;
	img->multi.length = 0;

#if defined(GIFLIB_MAJOR) && GIFLIB_MAJOR >= 5
	gif = DGifOpenFileName(file->path, NULL);
#else
	gif = DGifOpenFileName(file->path);
#endif
	if (gif == NULL) {
		error(0, 0, "%s: Error opening gif image", file->name);
		return false;
	}
	bg = gif->SBackGroundColor;
	sw = gif->SWidth;
	sh = gif->SHeight;
	px = py = pw = ph = 0;

	do {
		if (DGifGetRecordType(gif, &rec) == GIF_ERROR) {
			err = true;
			break;
		}
		if (rec == EXTENSION_RECORD_TYPE) {
			int ext_code;
			GifByteType *ext = NULL;

			DGifGetExtension(gif, &ext_code, &ext);
			while (ext) {
				if (ext_code == GRAPHICS_EXT_FUNC_CODE) {
					if (ext[1] & 1)
						transp = (int) ext[4];
					else
						transp = -1;

					delay = 10 * ((unsigned int) ext[3] << 8 | (unsigned int) ext[2]);
					disposal = (unsigned int) ext[1] >> 2 & 0x7;
				}
				ext = NULL;
				DGifGetExtensionNext(gif, &ext);
			}
		} else if (rec == IMAGE_DESC_RECORD_TYPE) {
			if (DGifGetImageDesc(gif) == GIF_ERROR) {
				err = true;
				break;
			}
			x = gif->Image.Left;
			y = gif->Image.Top;
			w = gif->Image.Width;
			h = gif->Image.Height;

			rows = (GifRowType*) emalloc(h * sizeof(GifRowType));
			for (i = 0; i < h; i++)
				rows[i] = (GifRowType) emalloc(w * sizeof(GifPixelType));
			if (gif->Image.Interlace) {
				for (i = 0; i < 4; i++) {
					for (j = intoffset[i]; j < h; j += intjump[i])
						DGifGetLine(gif, rows[j], w);
				}
			} else {
				for (i = 0; i < h; i++)
					DGifGetLine(gif, rows[i], w);
			}

			ptr = data = (DATA32*) emalloc(sizeof(DATA32) * sw * sh);
			cmap = gif->Image.ColorMap ? gif->Image.ColorMap : gif->SColorMap;
			if (bg >= cmap->ColorCount) {
				err = true;
				break;
			}
			r = cmap->Colors[bg].Red;
			g = cmap->Colors[bg].Green;
			b = cmap->Colors[bg].Blue;
			bgpixel = 0x00ffffff & (r << 16 | g << 8 | b);

			for (i = 0; i < sh; i++) {
				for (j = 0; j < sw; j++) {
					if (i < y || i >= y + h || j < x || j >= x + w ||
					    rows[i-y][j-x] == transp)
					{
						if (prev_frame != NULL && (prev_disposal != 2 ||
						    i < py || i >= py + ph || j < px || j >= px + pw))
						{
							*ptr = prev_frame[i * sw + j];
						} else {
							*ptr = bgpixel;
						}
					} else {
						r = cmap->Colors[rows[i-y][j-x]].Red;
						g = cmap->Colors[rows[i-y][j-x]].Green;
						b = cmap->Colors[rows[i-y][j-x]].Blue;
						*ptr = 0xffu << 24 | r << 16 | g << 8 | b;
					}
					ptr++;
				}
			}

			im = imlib_create_image_using_copied_data(sw, sh, data);

			for (i = 0; i < h; i++)
				free(rows[i]);
			free(rows);
			free(data);

			if (im == NULL) {
				err = true;
				break;
			}

			imlib_context_set_image(im);
			imlib_image_set_format("gif");
			if (transp >= 0)
				imlib_image_set_has_alpha(1);

			if (disposal != 3)
				prev_frame = imlib_image_get_data_for_reading_only();
			prev_disposal = disposal;
			px = x, py = y, pw = w, ph = h;

			if (img->multi.cnt == img->multi.cap) {
				img->multi.cap *= 2;
				img->multi.frames = (img_frame_t*)
				                    erealloc(img->multi.frames,
				                             img->multi.cap * sizeof(img_frame_t));
			}
			img->multi.frames[img->multi.cnt].im = im;
			delay = img->multi.framedelay > 0 ? img->multi.framedelay : delay;
			img->multi.frames[img->multi.cnt].delay = delay > 0 ? delay : DEF_GIF_DELAY;
			img->multi.length += img->multi.frames[img->multi.cnt].delay;
			img->multi.cnt++;
		}
	} while (rec != TERMINATE_RECORD_TYPE);

#if defined(GIFLIB_MAJOR) && GIFLIB_MAJOR >= 5 && GIFLIB_MINOR >= 1
	DGifCloseFile(gif, NULL);
#else
	DGifCloseFile(gif);
#endif

	if (err && (file->flags & FF_WARN))
		error(0, 0, "%s: Corrupted gif file", file->name);

	if (img->multi.cnt > 1) {
		imlib_context_set_image(img->im);
		imlib_free_image();
		img->im = img->multi.frames[0].im;
	} else if (img->multi.cnt == 1) {
		imlib_context_set_image(img->multi.frames[0].im);
		imlib_free_image();
		img->multi.cnt = 0;
	}

	imlib_context_set_image(img->im);

	return !err;
}
#endif /* HAVE_LIBGIF */


#if HAVE_LIBWEBP
bool is_webp(const char *path)
{
	/* The size (in bytes) of the largest amount of data required to verify a WebP image. */
	enum { max = 30 };
	const unsigned char fmt[max];
	bool ret = false;
	FILE *f;

	if ((f = fopen(path, "rb")) != NULL) {
		if (fread((unsigned char *) fmt, 1, max, f) == max)
			ret = WebPGetInfo(fmt, max, NULL, NULL);
		fclose(f);
	}
	return ret;
}

/* fframe   img
 * NULL     NULL  = do nothing
 * x        NULL  = load the first frame as an Imlib_Image
 * NULL     x     = load all frames into img->multi.
 */
bool img_load_webp(const fileinfo_t *file, Imlib_Image *fframe, img_t *img)
{
	FILE *webp_file;
	WebPData data;
	data.bytes = NULL;

	Imlib_Image im = NULL;
	struct WebPAnimDecoderOptions opts;
	WebPAnimDecoder *dec = NULL;
	struct WebPAnimInfo info;
	unsigned char *buf = NULL;
	int ts;
	const WebPDemuxer *demux;
	WebPIterator iter;
	unsigned long flags;
	unsigned int delay;
	bool err = false;

	if ((err = fframe == NULL && img == NULL))
		goto fail;

	if ((err = (webp_file = fopen(file->path, "rb")) == NULL)) {
		error(0, 0, "%s: Error opening webp image", file->name);
		goto fail;
	}
	fseek(webp_file, 0L, SEEK_END);
	data.size = ftell(webp_file);
	rewind(webp_file);
	data.bytes = emalloc(data.size);
	if ((err = fread((unsigned char *)data.bytes, 1, data.size, webp_file) != data.size)) {
		error(0, 0, "%s: Error reading webp image", file->name);
		goto fail;
	}

	/* Setup the WebP Animation Decoder */
	if ((err = !WebPAnimDecoderOptionsInit(&opts))) {
		error(0, 0, "%s: WebP library version mismatch", file->name);
		goto fail;
	}
	opts.color_mode = MODE_BGRA;
	/* NOTE: Multi-threaded decoding may cause problems on some system */
	opts.use_threads = true;
	dec = WebPAnimDecoderNew(&data, &opts);
	if ((err = (dec == NULL) || !WebPAnimDecoderGetInfo(dec, &info))) {
		error(0, 0, "%s: WebP parsing or memory error (file is corrupt?)", file->name);
		goto fail;
	}
	demux = WebPAnimDecoderGetDemuxer(dec);

	if (img == NULL) { /* Only get the first frame and put it into fframe. */
		if ((err = !WebPAnimDecoderGetNext(dec, &buf, &ts))) {
			error(0, 0, "%s: Error loading first frame", file->name);
			goto fail;
		}
		*fframe = imlib_create_image_using_copied_data(
		          info.canvas_width, info.canvas_height, (DATA32*)buf);
		imlib_context_set_image(*fframe);
		imlib_image_set_has_alpha(WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS) & ALPHA_FLAG);
	} else {
		/* Get global information for the image */
		flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
		img->w = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
		img->h = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
		img->multi.cap = info.frame_count;
		img->multi.sel = 0;
		img->multi.frames = emalloc(info.frame_count * sizeof(img_frame_t));

		/* Load and decode frames (also works on images with only 1 frame) */
		img->multi.cnt = 0;
		while (WebPAnimDecoderGetNext(dec, &buf, &ts)) {
			im = imlib_create_image_using_copied_data(
			     info.canvas_width, info.canvas_height, (DATA32*)buf);
			imlib_context_set_image(im);
			imlib_image_set_format("webp");
			/* Get an iterator of this frame - used for frame info (duration, etc.) */
			WebPDemuxGetFrame(demux, img->multi.cnt+1, &iter);
			imlib_image_set_has_alpha((flags & ALPHA_FLAG) == ALPHA_FLAG);
			/* Store info for this frame */
			img->multi.frames[img->multi.cnt].im = im;
			delay = iter.duration > 0 ? iter.duration : DEF_WEBP_DELAY;
			img->multi.frames[img->multi.cnt].delay = delay;
			img->multi.length += img->multi.frames[img->multi.cnt].delay;
			img->multi.cnt++;
		}
		WebPDemuxReleaseIterator(&iter);

		if (img->multi.cnt > 1) {
			imlib_context_set_image(img->im);
			imlib_free_image();
			img->im = img->multi.frames[0].im;
		} else if (img->multi.cnt == 1) {
			imlib_context_set_image(img->multi.frames[0].im);
			imlib_free_image();
			img->multi.cnt = 0;
		}
		imlib_context_set_image(img->im);
	}
	imlib_image_set_format("webp");
fail:
	if (dec != NULL)
		WebPAnimDecoderDelete(dec);
	free((unsigned char *)data.bytes);
	return !err;
}
#endif /* HAVE_LIBWEBP */

Imlib_Image img_open(const fileinfo_t *file)
{
	struct stat st;
	Imlib_Image im = NULL;

	if (access(file->path, R_OK) == 0 &&
	    stat(file->path, &st) == 0 && S_ISREG(st.st_mode))
	{
#if HAVE_LIBWEBP
		if (is_webp(file->path))
			img_load_webp(file, &im, NULL);
		else
#endif
			im = imlib_load_image(file->path);
		if (im != NULL) {
			imlib_context_set_image(im);
#if HAVE_LIBWEBP
			const char *fmt;
			if ((fmt = imlib_image_format()) != NULL && !STREQ(fmt, "webp") &&
			    imlib_image_get_data_for_reading_only() == NULL) {
#else
			if (imlib_image_get_data_for_reading_only() == NULL) {
#endif
				imlib_free_image();
				im = NULL;
			}
		}
	}
	if (im == NULL && (file->flags & FF_WARN))
		error(0, 0, "%s: Error opening image", file->name);
	return im;
}

bool img_load(img_t *img, const fileinfo_t *file)
{
	const char *fmt;

	if ((img->im = img_open(file)) == NULL)
		return false;

	imlib_image_set_changes_on_disk();

#if HAVE_LIBEXIF
	exif_auto_orientate(file);
#endif

	if ((fmt = imlib_image_format()) != NULL) {
#if HAVE_LIBGIF
		if (STREQ(fmt, "gif"))
			img_load_gif(img, file);
#endif
#if HAVE_LIBWEBP
		if (STREQ(fmt, "webp"))
			img_load_webp(file, NULL, img);
#endif
	}
	img->w = imlib_image_get_width();
	img->h = imlib_image_get_height();
	img->checkpan = true;
	img->dirty = true;

	return true;
}

CLEANUP void img_close(img_t *img, bool decache)
{
	int i;

	if (img->multi.cnt > 0) {
		for (i = 0; i < img->multi.cnt; i++) {
			imlib_context_set_image(img->multi.frames[i].im);
			imlib_free_image();
		}
		img->multi.cnt = 0;
		img->im = NULL;
	} else if (img->im != NULL) {
		imlib_context_set_image(img->im);
		if (decache)
			imlib_free_image_and_decache();
		else
			imlib_free_image();
		img->im = NULL;
	}
}

void img_check_pan(img_t *img, bool moved)
{
	win_t *win;
	float w, h, ox, oy;

	win = img->win;
	w = img->w * img->zoom;
	h = img->h * img->zoom;
	ox = img->x;
	oy = img->y;

	if (w < win->w)
		img->x = (win->w - w) / 2;
	else if (img->x > 0)
		img->x = 0;
	else if (img->x + w < win->w)
		img->x = win->w - w;
	if (h < win->h)
		img->y = (win->h - h) / 2;
	else if (img->y > 0)
		img->y = 0;
	else if (img->y + h < win->h)
		img->y = win->h - h;

	if (!moved && (ox != img->x || oy != img->y))
		img->dirty = true;
}

bool img_fit(img_t *img)
{
	float z, zw, zh;

	if (img->scalemode == SCALE_ZOOM)
		return false;

	zw = (float) img->win->w / (float) img->w;
	zh = (float) img->win->h / (float) img->h;

	switch (img->scalemode) {
		case SCALE_FILL:
			z = MAX(zw, zh);
			break;
		case SCALE_WIDTH:
			z = zw;
			break;
		case SCALE_HEIGHT:
			z = zh;
			break;
		default:
			z = MIN(zw, zh);
			break;
	}
	z = MIN(z, img->scalemode == SCALE_DOWN ? 1.0 : ZOOM_MAX);

	if (ABS(img->zoom - z) > 1.0/MAX(img->w, img->h)) {
		img->zoom = z;
		img->dirty = true;
		return true;
	} else {
		return false;
	}
}

void img_render(img_t *img)
{
	win_t *win;
	int sx, sy, sw, sh;
	int dx, dy, dw, dh;
	Imlib_Image bg;
	XColor bgc;

	win = img->win;
	img_fit(img);

	if (img->checkpan) {
		img_check_pan(img, false);
		img->checkpan = false;
	}

	if (!img->dirty)
		return;

	/* calculate source and destination offsets:
	 *   - part of image drawn on full window, or
	 *   - full image drawn on part of window
	 */
	if (img->x <= 0) {
		sx = -img->x / img->zoom + 0.5;
		sw = win->w / img->zoom;
		dx = 0;
		dw = win->w;
	} else {
		sx = 0;
		sw = img->w;
		dx = img->x;
		dw = img->w * img->zoom;
	}
	if (img->y <= 0) {
		sy = -img->y / img->zoom + 0.5;
		sh = win->h / img->zoom;
		dy = 0;
		dh = win->h;
	} else {
		sy = 0;
		sh = img->h;
		dy = img->y;
		dh = img->h * img->zoom;
	}

	win_clear(win);

	imlib_context_set_image(img->im);
	imlib_context_set_anti_alias(img->aa);
	imlib_context_set_drawable(win->buf.pm);

	if (imlib_image_has_alpha()) {
		if ((bg = imlib_create_image(dw, dh)) == NULL)
			error(EXIT_FAILURE, ENOMEM, NULL);
		imlib_context_set_image(bg);
		imlib_image_set_has_alpha(1);

		if (img->alpha) {
			int i, c, r;
			DATA32 col[2] = { 0xFF666666, 0xFF999999 };
			DATA32 * data = imlib_image_get_data();

			for (r = 0; r < dh; r++) {
				i = r * dw;
				if (r == 0 || r == 8) {
					for (c = 0; c < dw; c++)
						data[i++] = col[!(c & 8) ^ !r];
				} else {
					memcpy(&data[i], &data[(r & 8) * dw], dw * sizeof(data[0]));
				}
			}
			imlib_image_put_back_data(data);
		} else {
			bgc = win->win_bg;
			imlib_context_set_color(bgc.red >> 8, bgc.green >> 8, bgc.blue >> 8, 0xFF);
			imlib_image_fill_rectangle(0, 0, dw, dh);
		}
		imlib_blend_image_onto_image(img->im, 0, sx, sy, sw, sh, 0, 0, dw, dh);
		imlib_context_set_color_modifier(NULL);
		imlib_render_image_on_drawable(dx, dy);
		imlib_free_image();
		imlib_context_set_color_modifier(img->cmod);
	} else {
		imlib_image_set_has_alpha(1);
		imlib_render_image_part_on_drawable_at_size(sx, sy, sw, sh, dx, dy, dw, dh);
	}
	img->dirty = false;
}

bool img_fit_win(img_t *img, scalemode_t sm)
{
	float oz;

	oz = img->zoom;
	img->scalemode = sm;

	if (img_fit(img)) {
		img->x = img->win->w / 2 - (img->win->w / 2 - img->x) * img->zoom / oz;
		img->y = img->win->h / 2 - (img->win->h / 2 - img->y) * img->zoom / oz;
		img->checkpan = true;
		return true;
	} else {
		return false;
	}
}

bool img_zoom_to(img_t *img, float z)
{
	int x, y;
	if (ZOOM_MIN <= z && z <= ZOOM_MAX) {
		win_cursor_pos(img->win, &x, &y);
		if (x < 0 || x >= img->win->w || y < 0 || y >= img->win->h) {
			x = img->win->w / 2;
			y = img->win->h / 2;
		}
		img->x = x - (x - img->x) * z / img->zoom;
		img->y = y - (y - img->y) * z / img->zoom;
		img->zoom = z;
		img->scalemode = SCALE_ZOOM;
		img->checkpan = true;
		img->dirty = true;
		return true;
	} else {
		return false;
	}
}

bool img_zoom(img_t *img, int d)
{
	const float z = img->zoom * (d > 0 ? ZOOM_STEP : 1/ZOOM_STEP);
	return img_zoom_to(img, z);
}

bool img_pos(img_t *img, float x, float y)
{
	float ox, oy;

	ox = img->x;
	oy = img->y;

	img->x = x;
	img->y = y;

	img_check_pan(img, true);

	if (ox != img->x || oy != img->y) {
		img->dirty = true;
		return true;
	} else {
		return false;
	}
}

bool img_move(img_t *img, float dx, float dy)
{
	return img_pos(img, img->x + dx, img->y + dy);
}

bool img_pan(img_t *img, direction_t dir, int d)
{
	/* d < 0: screen-wise
	 * d = 0: 1/PAN_FRACTION of screen
	 * d > 0: num of pixels
	 */
	float x, y;

	if (d > 0) {
		x = y = MAX(1, (float) d * img->zoom);
	} else {
		x = img->win->w / (d < 0 ? 1 : PAN_FRACTION);
		y = img->win->h / (d < 0 ? 1 : PAN_FRACTION);
	}

	switch (dir) {
		case DIR_LEFT:
			return img_move(img, x, 0.0);
		case DIR_RIGHT:
			return img_move(img, -x, 0.0);
		case DIR_UP:
			return img_move(img, 0.0, y);
		case DIR_DOWN:
			return img_move(img, 0.0, -y);
	}
	return false;
}

bool img_pan_edge(img_t *img, direction_t dir)
{
	float ox, oy;

	ox = img->x;
	oy = img->y;

	if (dir & DIR_LEFT)
		img->x = 0;
	if (dir & DIR_RIGHT)
		img->x = img->win->w - img->w * img->zoom;
	if (dir & DIR_UP)
		img->y = 0;
	if (dir & DIR_DOWN)
		img->y = img->win->h - img->h * img->zoom;

	img_check_pan(img, true);

	if (ox != img->x || oy != img->y) {
		img->dirty = true;
		return true;
	} else {
		return false;
	}
}

void img_rotate(img_t *img, degree_t d)
{
	int i, tmp;
	float ox, oy;

	imlib_context_set_image(img->im);
	imlib_image_orientate(d);

	for (i = 0; i < img->multi.cnt; i++) {
		if (i != img->multi.sel) {
			imlib_context_set_image(img->multi.frames[i].im);
			imlib_image_orientate(d);
		}
	}
	if (d == DEGREE_90 || d == DEGREE_270) {
		ox = d == DEGREE_90  ? img->x : img->win->w - img->x - img->w * img->zoom;
		oy = d == DEGREE_270 ? img->y : img->win->h - img->y - img->h * img->zoom;

		img->x = oy + (img->win->w - img->win->h) / 2;
		img->y = ox + (img->win->h - img->win->w) / 2;

		tmp = img->w;
		img->w = img->h;
		img->h = tmp;
		img->checkpan = true;
	}
	img->dirty = true;
}

void img_flip(img_t *img, flipdir_t d)
{
	int i;
	void (*imlib_flip_op[3])(void) = {
		imlib_image_flip_horizontal,
		imlib_image_flip_vertical,
		imlib_image_flip_diagonal
	};

	d = (d & (FLIP_HORIZONTAL | FLIP_VERTICAL)) - 1;

	if (d < 0 || d >= ARRLEN(imlib_flip_op))
		return;

	imlib_context_set_image(img->im);
	imlib_flip_op[d]();

	for (i = 0; i < img->multi.cnt; i++) {
		if (i != img->multi.sel) {
			imlib_context_set_image(img->multi.frames[i].im);
			imlib_flip_op[d]();
		}
	}
	img->dirty = true;
}

void img_toggle_antialias(img_t *img)
{
	img->aa = !img->aa;
	imlib_context_set_image(img->im);
	imlib_context_set_anti_alias(img->aa);
	img->dirty = true;
}

bool img_change_gamma(img_t *img, int d)
{
	/* d < 0: decrease gamma
	 * d = 0: reset gamma
	 * d > 0: increase gamma
	 */
	int gamma;
	double range;

	if (d == 0)
		gamma = 0;
	else
		gamma = MIN(MAX(img->gamma + d, -GAMMA_RANGE), GAMMA_RANGE);

	if (img->gamma != gamma) {
		imlib_reset_color_modifier();
		if (gamma != 0) {
			range = gamma <= 0 ? 1.0 : GAMMA_MAX - 1.0;
			imlib_modify_color_modifier_gamma(1.0 + gamma * (range / GAMMA_RANGE));
		}
		img->gamma = gamma;
		img->dirty = true;
		return true;
	} else {
		return false;
	}
}

bool img_frame_goto(img_t *img, int n)
{
	if (n < 0 || n >= img->multi.cnt || n == img->multi.sel)
		return false;

	img->multi.sel = n;
	img->im = img->multi.frames[n].im;

	imlib_context_set_image(img->im);
	img->w = imlib_image_get_width();
	img->h = imlib_image_get_height();
	img->checkpan = true;
	img->dirty = true;

	return true;
}

bool img_frame_navigate(img_t *img, int d)
{
	if (img->multi.cnt == 0 || d == 0)
		return false;

	d += img->multi.sel;
	if (d < 0)
		d = 0;
	else if (d >= img->multi.cnt)
		d = img->multi.cnt - 1;

	return img_frame_goto(img, d);
}

bool img_frame_animate(img_t *img)
{
	if (img->multi.cnt == 0)
		return false;

	if (img->multi.sel + 1 >= img->multi.cnt)
		img_frame_goto(img, 0);
	else
		img_frame_goto(img, img->multi.sel + 1);
	img->dirty = true;
	return true;
}

