/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * UFOs HD: high-resolution presentation layer for the Gnap engine.
 *
 * Game logic keeps working in the original 800x600 coordinate space with
 * the original surfaces. Every surface additionally gets an HD "twin"
 * (scale x bigger). All drawing operations are mirrored into the twins,
 * using replacement artwork from the HD folder when present, otherwise an
 * upscaled copy of the original graphics. The screen is fed from the twins.
 */

#ifndef GNAP_HDLAYER_H
#define GNAP_HDLAYER_H

#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/list.h"
#include "common/fs.h"
#include "common/rect.h"
#include "common/str.h"
#include "graphics/surface.h"
#include "graphics/cursor.h"

namespace Gnap {

class GnapEngine;
class SpriteResource;

class HDLayer {
public:
	HDLayer(GnapEngine *vm, int scale);
	~HDLayer();

	int scale() const { return _scale; }
	bool enabled() const { return _scale > 1; }
	Common::Rect up(const Common::Rect &r) const {
		return Common::Rect(r.left * _scale, r.top * _scale, r.right * _scale, r.bottom * _scale);
	}

	// --- surface twins -------------------------------------------------
	Graphics::Surface *twin(Graphics::Surface *s);            // created lazily by upscaling s
	void setTwin(Graphics::Surface *s, Graphics::Surface *hd); // takes ownership of hd
	void resetTwin(Graphics::Surface *s, bool transparentFill);
	void forget(Graphics::Surface *s);
	void syncRect(Graphics::Surface *s, const Common::Rect &r1x); // re-upscale a 1x area

	// --- resources -------------------------------------------------------
	// HD version of a sprite resource (cached, owned by the layer)
	const Graphics::Surface *sprite(int resourceId, SpriteResource *res);
	// HD replacement for a background bitmap, or nullptr (caller owns result)
	Graphics::Surface *loadBitmapOverride(int resourceId, int w, int h);
	void purgeSprites();
	// Optional per-sprite position offsets (1x pixels) from HD/<DAT>/offsets.txt,
	// lines "NNN dx dy". Used when redrawn backgrounds moved a detail.
	void adjustFrameRect(int resourceId, Common::Rect &r);

	// --- drawing (all rects in HD coordinates) ---------------------------
	void blit(Graphics::Surface *dst, const Common::Rect &frame, const Common::Rect &clip,
		const Graphics::Surface *src, bool forceOpaque);
	void copyRect(Graphics::Surface *dst, const Graphics::Surface *src, const Common::Rect &r);
	void fillRect(Graphics::Surface *dst, const Common::Rect &r, uint32 color);

	// --- screen ------------------------------------------------------------
	void markDirty(const Common::Rect &rHD);
	void present(Graphics::Surface *frontHD);
	void presentAll(Graphics::Surface *frontHD);
	// Upscale an arbitrary 1x image (screen format) and put it on screen at 1x position.
	void drawScaledToScreen(const Graphics::Surface *img, const Common::Rect &dest1x);

	// --- cursor --------------------------------------------------------------
	void replaceCursor(const Graphics::Cursor *cursor);

	// --- video override --------------------------------------------------------
	Common::FSNode videoOverride(const char *name) const;

	static Graphics::Surface *resample(const Graphics::Surface *src, int w, int h);

private:
	struct CacheEntry {
		Graphics::Surface *surface;
		uint32 bytes;
	};
	struct PtrHash {
		uint operator()(const Graphics::Surface *p) const { return (uint)(((uintptr)p) >> 4); }
	};
	typedef Common::HashMap<Graphics::Surface *, Graphics::Surface *, PtrHash> TwinMap;
	typedef Common::HashMap<Common::String, CacheEntry> SpriteMap;
	typedef Common::HashMap<int, Common::FSNode> FileIndex;
	typedef Common::HashMap<int, Common::Point> OffsetMap;

	GnapEngine *_vm;
	int _scale;
	TwinMap _twins;
	SpriteMap _sprites;
	Common::List<Common::String> _lru;
	uint32 _cacheBytes, _cacheLimit;
	Common::HashMap<Common::String, FileIndex> _fileIndex;
	Common::HashMap<Common::String, OffsetMap> _offsets;
	Common::FSNode _hdRoot;
	bool _hdRootValid;
	Common::Rect _dirty;
	uint32 _presentCount;
	Graphics::Surface _scratch;

	Graphics::Surface *upscale(const Graphics::Surface *s) const;
	Graphics::Surface *newSurface(int w, int h, bool transparent) const;
	const FileIndex &indexFor(const Common::String &datName);
	Graphics::Surface *loadPng(const Common::FSNode &node) const;
	Common::String keyFor(int resourceId, int *entry, Common::String *datName) const;
	void touch(const Common::String &key);
	void evict();
};

} // End of namespace Gnap

#endif
