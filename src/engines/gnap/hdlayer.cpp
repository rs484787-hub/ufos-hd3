/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * UFOs HD: high-resolution presentation layer for the Gnap engine.
 */

#include "gnap/hdlayer.h"
#include "gnap/gnap.h"
#include "gnap/resource.h"

#include "common/array.h"
#include "common/config-manager.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/stream.h"
#include "common/system.h"
#include "graphics/cursorman.h"
#include "graphics/palette.h"
#ifdef USE_PNG
#include "image/png.h"
#endif

namespace Gnap {

static Graphics::PixelFormat screenFormat() {
	return Graphics::PixelFormat::createFormatABGR32();
}

// --- pixel helpers (32 bpp, format = screenFormat()) -----------------------

static inline uint32 alphaOf(uint32 p, const Graphics::PixelFormat &f) {
	return (p >> f.aShift) & 0xFF;
}

static inline uint32 withAlpha(uint32 p, uint32 a, const Graphics::PixelFormat &f) {
	return (p & ~(0xFFu << f.aShift)) | (a << f.aShift);
}

// Porter-Duff "source over destination" for straight (non-premultiplied) alpha
static inline uint32 over(uint32 s, uint32 d, const Graphics::PixelFormat &f) {
	const uint32 sa = alphaOf(s, f);
	if (sa == 255)
		return s;
	if (sa == 0)
		return d;
	const uint32 da = alphaOf(d, f);
	const uint32 oa = sa + (da * (255 - sa) + 127) / 255;
	if (oa == 0)
		return d;
	uint32 out = 0;
	const int shifts[3] = { f.rShift, f.gShift, f.bShift };
	for (int i = 0; i < 3; ++i) {
		const uint32 sc = (s >> shifts[i]) & 0xFF;
		const uint32 dc = (d >> shifts[i]) & 0xFF;
		const uint32 c = (sc * sa * 255 + dc * da * (255 - sa) + oa * 255 / 2) / (oa * 255);
		out |= MIN<uint32>(c, 255) << shifts[i];
	}
	return out | (oa << f.aShift);
}

// ---------------------------------------------------------------------------

HDLayer::HDLayer(GnapEngine *vm, int scale) : _vm(vm), _scale(scale), _cacheBytes(0), _hdRootValid(false), _presentCount(0) {
	int limitMb = ConfMan.hasKey("hd_cache_mb") ? ConfMan.getInt("hd_cache_mb") : 768;
	_cacheLimit = (uint32)CLIP(limitMb, 64, 3072) * 1024 * 1024;

	// Where to look for replacement artwork, first match wins:
	//   hd_path from the config, ./HD (next to UFOs.exe), <game>/../HD, <game>/HD
	Common::Array<Common::FSNode> candidates;
	if (ConfMan.hasKey("hd_path") && !ConfMan.get("hd_path").empty())
		candidates.push_back(Common::FSNode(ConfMan.getPath("hd_path")));
	candidates.push_back(Common::FSNode(Common::Path("HD")));
	Common::FSNode game(ConfMan.getPath("path"));
	candidates.push_back(game.getParent().getChild("HD"));
	candidates.push_back(game.getChild("HD"));
	for (uint i = 0; i < candidates.size() && !_hdRootValid; ++i) {
		if (candidates[i].exists() && candidates[i].isDirectory()) {
			_hdRoot = candidates[i];
			_hdRootValid = true;
		}
	}
	debug(1, "UFOs HD: scale %d, HD folder %s (%s)", _scale,
		_hdRoot.getPath().toString(Common::Path::kNativeSeparator).c_str(), _hdRootValid ? "found" : "not found");
}

HDLayer::~HDLayer() {
	for (TwinMap::iterator it = _twins.begin(); it != _twins.end(); ++it) {
		it->_value->free();
		delete it->_value;
	}
	purgeSprites();
	_scratch.free();
}

Graphics::Surface *HDLayer::newSurface(int w, int h, bool transparent) const {
	const Graphics::PixelFormat f = screenFormat();
	Graphics::Surface *s = new Graphics::Surface();
	s->create(MAX(w, 1), MAX(h, 1), f);
	// same "transparent" value the engine uses for empty surfaces (alpha = 0)
	s->fillRect(Common::Rect(0, 0, s->w, s->h), transparent ? 0xFFFFFF00 : f.ARGBToColor(255, 0, 0, 0));
	return s;
}

Graphics::Surface *HDLayer::upscale(const Graphics::Surface *s) const {
	Graphics::Surface *hd = newSurface(s->w * _scale, s->h * _scale, true);
	for (int y = 0; y < hd->h; ++y) {
		const uint32 *src = (const uint32 *)s->getBasePtr(0, y / _scale);
		uint32 *dst = (uint32 *)hd->getBasePtr(0, y);
		for (int x = 0; x < hd->w; ++x)
			dst[x] = src[x / _scale];
	}
	return hd;
}

// Area-averaging (down) / bilinear-ish (up) resampler with alpha weighting.
Graphics::Surface *HDLayer::resample(const Graphics::Surface *src, int w, int h) {
	const Graphics::PixelFormat f = src->format;
	Graphics::Surface *dst = new Graphics::Surface();
	dst->create(w, h, f);
	if (src->w == w && src->h == h) {
		dst->copyFrom(*src);
		return dst;
	}
	const double sx = (double)src->w / w, sy = (double)src->h / h;
	for (int y = 0; y < h; ++y) {
		uint32 *out = (uint32 *)dst->getBasePtr(0, y);
		double y0 = y * sy, y1 = (y + 1) * sy;
		if (sy < 1.0) { y0 = (y + 0.5) * sy - 0.5; y1 = y0 + 1.0; }
		for (int x = 0; x < w; ++x) {
			double x0 = x * sx, x1 = (x + 1) * sx;
			if (sx < 1.0) { x0 = (x + 0.5) * sx - 0.5; x1 = x0 + 1.0; }
			double acc[4] = { 0, 0, 0, 0 }, wsum = 0;
			const int iy0 = MAX(0, (int)floor(y0)), iy1 = MIN(src->h - 1, (int)ceil(y1) - 1);
			const int ix0 = MAX(0, (int)floor(x0)), ix1 = MIN(src->w - 1, (int)ceil(x1) - 1);
			for (int yy = iy0; yy <= iy1; ++yy) {
				const double wy = MIN(y1, yy + 1.0) - MAX(y0, (double)yy);
				if (wy <= 0) continue;
				const uint32 *row = (const uint32 *)src->getBasePtr(0, yy);
				for (int xx = ix0; xx <= ix1; ++xx) {
					const double wx = MIN(x1, xx + 1.0) - MAX(x0, (double)xx);
					if (wx <= 0) continue;
					const double wgt = wx * wy;
					const uint32 p = row[xx];
					const double a = ((p >> f.aShift) & 0xFF) / 255.0;
					acc[0] += ((p >> f.rShift) & 0xFF) * a * wgt;
					acc[1] += ((p >> f.gShift) & 0xFF) * a * wgt;
					acc[2] += ((p >> f.bShift) & 0xFF) * a * wgt;
					acc[3] += a * wgt;
					wsum += wgt;
				}
			}
			if (wsum <= 0 || acc[3] <= 0) {
				out[x] = 0xFFFFFF00;   // fully transparent
				continue;
			}
			const uint32 r = (uint32)MIN(255.0, acc[0] / acc[3] + 0.5);
			const uint32 g = (uint32)MIN(255.0, acc[1] / acc[3] + 0.5);
			const uint32 b = (uint32)MIN(255.0, acc[2] / acc[3] + 0.5);
			const uint32 a = (uint32)MIN(255.0, acc[3] / wsum * 255.0 + 0.5);
			out[x] = (r << f.rShift) | (g << f.gShift) | (b << f.bShift) | (a << f.aShift);
		}
	}
	return dst;
}

// --- twins ------------------------------------------------------------------

Graphics::Surface *HDLayer::twin(Graphics::Surface *s) {
	if (!s)
		return nullptr;
	TwinMap::iterator it = _twins.find(s);
	if (it != _twins.end() && it->_value->w == s->w * _scale && it->_value->h == s->h * _scale)
		return it->_value;
	Graphics::Surface *hd = upscale(s);
	setTwin(s, hd);
	return hd;
}

void HDLayer::setTwin(Graphics::Surface *s, Graphics::Surface *hd) {
	TwinMap::iterator it = _twins.find(s);
	if (it != _twins.end() && it->_value != hd) {
		it->_value->free();
		delete it->_value;
	}
	_twins[s] = hd;
}

void HDLayer::resetTwin(Graphics::Surface *s, bool transparentFill) {
	setTwin(s, newSurface(s->w * _scale, s->h * _scale, transparentFill));
}

void HDLayer::forget(Graphics::Surface *s) {
	TwinMap::iterator it = _twins.find(s);
	if (it != _twins.end()) {
		it->_value->free();
		delete it->_value;
		_twins.erase(it);
	}
}

void HDLayer::syncRect(Graphics::Surface *s, const Common::Rect &r1x) {
	Common::Rect r = r1x;
	r.clip(Common::Rect(0, 0, s->w, s->h));
	if (r.isEmpty())
		return;
	Graphics::Surface *hd = twin(s);
	for (int y = r.top * _scale; y < r.bottom * _scale; ++y) {
		const uint32 *src = (const uint32 *)s->getBasePtr(0, y / _scale);
		uint32 *dst = (uint32 *)hd->getBasePtr(0, y);
		for (int x = r.left * _scale; x < r.right * _scale; ++x)
			dst[x] = src[x / _scale];
	}
}

// --- resources -----------------------------------------------------------------

Common::String HDLayer::keyFor(int resourceId, int *entry, Common::String *datName) const {
	const int datIndex = (resourceId >> 16) & 0xFFFF;
	*entry = resourceId & 0xFFFF;
	*datName = _vm->_dat->getDatName(datIndex);
	return Common::String::format("%s/%d", datName->c_str(), *entry);
}

const HDLayer::FileIndex &HDLayer::indexFor(const Common::String &datName) {
	Common::HashMap<Common::String, FileIndex>::iterator it = _fileIndex.find(datName);
	if (it != _fileIndex.end())
		return it->_value;
	FileIndex &idx = _fileIndex[datName];
	if (_hdRootValid && !datName.empty()) {
		Common::FSNode dir = _hdRoot.getChild(datName);
		Common::FSList files;
		if (dir.exists() && dir.isDirectory() && dir.getChildren(files, Common::FSNode::kListFilesOnly)) {
			for (Common::FSList::const_iterator f = files.begin(); f != files.end(); ++f) {
				Common::String n = f->getName();
				n.toLowercase();
				if (!n.hasSuffix(".png"))
					continue;
				int num = 0, digits = 0;
				while (digits < (int)n.size() && Common::isDigit(n[digits])) {
					num = num * 10 + (n[digits] - '0');
					++digits;
				}
				if (digits > 0 && (n[digits] == '_' || n[digits] == '.'))
					idx[num] = *f;
			}
			debug(1, "UFOs HD: %d replacement image(s) in HD/%s", idx.size(), datName.c_str());
		}
	}
	return idx;
}

Graphics::Surface *HDLayer::loadPng(const Common::FSNode &node) const {
#ifdef USE_PNG
	Common::SeekableReadStream *stream = node.createReadStream();
	if (!stream)
		return nullptr;
	Image::PNGDecoder png;
	Graphics::Surface *result = nullptr;
	if (png.loadStream(*stream) && png.getSurface()) {
		const Graphics::Palette &pal = png.getPalette();
		result = png.getSurface()->convertTo(screenFormat(), pal.data(), pal.size());
	} else {
		warning("UFOs HD: could not read %s", node.getPath().toString(Common::Path::kNativeSeparator).c_str());
	}
	delete stream;
	return result;
#else
	return nullptr;
#endif
}

void HDLayer::touch(const Common::String &key) {
	for (Common::List<Common::String>::iterator it = _lru.begin(); it != _lru.end(); ++it) {
		if (*it == key) {
			_lru.erase(it);
			break;
		}
	}
	_lru.push_back(key);
}

void HDLayer::evict() {
	while (_cacheBytes > _cacheLimit && _lru.size() > 1) {
		Common::String key = _lru.front();
		_lru.pop_front();
		SpriteMap::iterator it = _sprites.find(key);
		if (it != _sprites.end()) {
			_cacheBytes -= it->_value.bytes;
			it->_value.surface->free();
			delete it->_value.surface;
			_sprites.erase(it);
		}
	}
}

void HDLayer::purgeSprites() {
	for (SpriteMap::iterator it = _sprites.begin(); it != _sprites.end(); ++it) {
		it->_value.surface->free();
		delete it->_value.surface;
	}
	_sprites.clear();
	_lru.clear();
	_cacheBytes = 0;
}

const Graphics::Surface *HDLayer::sprite(int resourceId, SpriteResource *res) {
	int entry;
	Common::String datName;
	const Common::String key = keyFor(resourceId, &entry, &datName);
	SpriteMap::iterator it = _sprites.find(key);
	if (it != _sprites.end()) {
		touch(key);
		return it->_value.surface;
	}

	const int w = res->_width * _scale, h = res->_height * _scale;
	Graphics::Surface *hd = nullptr;

	const FileIndex &idx = indexFor(datName);
	FileIndex::const_iterator f = idx.find(entry);
	if (f != idx.end()) {
		Graphics::Surface *png = loadPng(f->_value);
		if (png) {
			hd = resample(png, w, h);
			png->free();
			delete png;
		}
	}

	if (!hd) {
		// Upscale the original paletted sprite. Index 0 keeps its colour but gets
		// alpha 0, so both transparent and opaque drawing modes work.
		const Graphics::PixelFormat fmt = screenFormat();
		hd = newSurface(w, h, true);
		const int pitch = (res->_width + 3) & ~3;
		for (int y = 0; y < h; ++y) {
			const byte *src = res->_pixels + (y / _scale) * pitch;
			uint32 *dst = (uint32 *)hd->getBasePtr(0, y);
			for (int x = 0; x < w; ++x) {
				const byte idxPx = src[x / _scale];
				const uint32 rgb = res->_palette[idxPx];
				byte px[4];
				px[0] = idxPx ? 0xFF : 0x00;
				px[1] = rgb & 0xFF;
				px[2] = (rgb >> 8) & 0xFF;
				px[3] = (rgb >> 16) & 0xFF;
				dst[x] = READ_UINT32(px);
			}
		}
		(void)fmt;
	}

	CacheEntry ce;
	ce.surface = hd;
	ce.bytes = hd->pitch * hd->h;
	_sprites[key] = ce;
	_cacheBytes += ce.bytes;
	touch(key);
	evict();
	return _sprites[key].surface;
}

Graphics::Surface *HDLayer::loadBitmapOverride(int resourceId, int w, int h) {
	int entry;
	Common::String datName;
	keyFor(resourceId, &entry, &datName);
	debug(1, "UFOs HD: background %s/%03d", datName.c_str(), entry);
	const FileIndex &idx = indexFor(datName);
	FileIndex::const_iterator f = idx.find(entry);
	if (f == idx.end())
		return nullptr;
	Graphics::Surface *png = loadPng(f->_value);
	if (!png)
		return nullptr;
	Graphics::Surface *hd = resample(png, w * _scale, h * _scale);
	png->free();
	delete png;
	// backgrounds are opaque
	const Graphics::PixelFormat fmt = hd->format;
	for (int y = 0; y < hd->h; ++y) {
		uint32 *row = (uint32 *)hd->getBasePtr(0, y);
		for (int x = 0; x < hd->w; ++x)
			row[x] = withAlpha(row[x], 255, fmt);
	}
	return hd;
}

// --- drawing -------------------------------------------------------------------

void HDLayer::blit(Graphics::Surface *dst, const Common::Rect &frame, const Common::Rect &clipIn,
	const Graphics::Surface *src, bool forceOpaque) {
	if (!dst || !src || frame.isEmpty())
		return;
	Common::Rect clip = clipIn;
	clip.clip(frame);
	clip.clip(Common::Rect(0, 0, dst->w, dst->h));
	if (clip.isEmpty())
		return;
	const Graphics::PixelFormat f = dst->format;
	const int fw = frame.width(), fh = frame.height();
	const bool oneToOne = (fw == src->w && fh == src->h);
	for (int y = clip.top; y < clip.bottom; ++y) {
		const int sy = oneToOne ? y - frame.top : (int)(((int64)(y - frame.top) * src->h) / fh);
		const uint32 *srow = (const uint32 *)src->getBasePtr(0, CLIP(sy, 0, src->h - 1));
		uint32 *drow = (uint32 *)dst->getBasePtr(0, y);
		for (int x = clip.left; x < clip.right; ++x) {
			const int sx = oneToOne ? x - frame.left : (int)(((int64)(x - frame.left) * src->w) / fw);
			const uint32 p = srow[CLIP(sx, 0, src->w - 1)];
			if (forceOpaque)
				drow[x] = withAlpha(p, 255, f);
			else
				drow[x] = over(p, drow[x], f);
		}
	}
}

void HDLayer::copyRect(Graphics::Surface *dst, const Graphics::Surface *src, const Common::Rect &rIn) {
	Common::Rect r = rIn;
	r.clip(Common::Rect(0, 0, MIN(dst->w, src->w), MIN(dst->h, src->h)));
	if (r.isEmpty())
		return;
	const int bytes = r.width() * 4;
	for (int y = r.top; y < r.bottom; ++y)
		memcpy(dst->getBasePtr(r.left, y), src->getBasePtr(r.left, y), bytes);
}

void HDLayer::fillRect(Graphics::Surface *dst, const Common::Rect &rIn, uint32 color) {
	Common::Rect r = rIn;
	r.clip(Common::Rect(0, 0, dst->w, dst->h));
	if (!r.isEmpty())
		dst->fillRect(r, color);
}

// --- screen ----------------------------------------------------------------------

void HDLayer::markDirty(const Common::Rect &rHD) {
	if (rHD.isEmpty())
		return;
	if (_dirty.isEmpty())
		_dirty = rHD;
	else
		_dirty.extend(rHD);
}

void HDLayer::present(Graphics::Surface *frontHD) {
	if (!frontHD)
		return;
#ifdef USE_PNG
	// Hidden diagnostics option: hd_debug_dump=<folder> writes every 50th HD frame as PNG
	if (ConfMan.hasKey("hd_debug_dump") && (++_presentCount % 50) == 0) {
		Common::DumpFile out;
		Common::Path p = ConfMan.getPath("hd_debug_dump").appendComponent(Common::String::format("hd_%05d.png", _presentCount / 50));
		if (out.open(p, true))
			Image::writePNG(out, *frontHD);
	}
#endif
	Common::Rect r = _dirty;
	r.clip(Common::Rect(0, 0, frontHD->w, frontHD->h));
	_dirty = Common::Rect();
	if (r.isEmpty())
		return;
	g_system->copyRectToScreen(frontHD->getBasePtr(r.left, r.top), frontHD->pitch, r.left, r.top, r.width(), r.height());
}

void HDLayer::presentAll(Graphics::Surface *frontHD) {
	if (!frontHD)
		return;
	_dirty = Common::Rect();
	g_system->copyRectToScreen(frontHD->getPixels(), frontHD->pitch, 0, 0, frontHD->w, frontHD->h);
}

void HDLayer::drawScaledToScreen(const Graphics::Surface *img, const Common::Rect &dest1x) {
	const Common::Rect d = up(dest1x);
	if (d.isEmpty() || img->format.bytesPerPixel != 4)
		return;
	Graphics::Surface *scaled = resample(img, d.width(), d.height());
	g_system->copyRectToScreen(scaled->getPixels(), scaled->pitch, d.left, d.top, scaled->w, scaled->h);
	scaled->free();
	delete scaled;
}

// --- cursor --------------------------------------------------------------------------

void HDLayer::replaceCursor(const Graphics::Cursor *cursor) {
	CursorMan.replaceCursor(cursor, intToFrac(_scale), intToFrac(_scale));
}

// --- video -------------------------------------------------------------------------------

Common::FSNode HDLayer::videoOverride(const char *name) const {
	if (!_hdRootValid)
		return Common::FSNode();
	Common::FSNode n = _hdRoot.getChild("video").getChild(name);
	return (n.exists() && !n.isDirectory()) ? n : Common::FSNode();
}

} // End of namespace Gnap
