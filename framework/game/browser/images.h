#pragma once
#include "transport.h"
namespace tsuchi { namespace browser {
// Thumbnail: screenshot-strip tiles and avatars (small images, kept as decoded).
// Preview: one full screenshot; large originals are reduced before they are kept.
// Card: the picture on a listing card, about 530 px wide as served and kept at
// the width a card shows it.
enum class ImageKind : uint32_t { Thumbnail, Preview, Card };
enum class ImageState : uint32_t { Missing, Waiting, Loading, Ready, Failed };
struct ImageSize { uint32_t width=0, height=0; };
// Compressed envelope validation precedes the game's decoder. No decoder runs
// on a network worker; upload must validate/pin the returned UObject identity.
bool image_envelope(const uint8_t*, uint32_t, ImageKind, ImageSize*);
// The game's decoder reads PNG and JPEG; many avatars are GIFs. This decodes
// the first frame of a GIF of at most 512x512 into BGRA (four bytes a pixel,
// transparent pixels all zero). Every read and write is bounded; malformed
// data returns false. `capacity` is the size of `bgra` in bytes.
bool gif_first_frame(const uint8_t* data, uint32_t bytes, uint8_t* bgra, uint32_t capacity, ImageSize* out);
// The size a decoded image is kept at: previews above 1280x720 are reduced by
// the smallest whole factor that fits, card pictures wider than CARD_WIDTH are
// resampled to that width, and thumbnails are kept as decoded.
ImageSize image_stored_size(const ImageSize& decoded, ImageKind);
// The whole factor behind image_stored_size: each kept pixel covers
// factor x factor decoded pixels. 1 when the image is kept as decoded.
uint32_t image_reduction(const ImageSize& decoded, ImageKind);
struct TextureToken { uint64_t object=0; uint32_t index=0, serial=0; };
struct ImageBackend {
    void* context=nullptr;
    // Decodes `decoded` pixels and keeps a texture of image_stored_size().
    bool (*upload)(void*, const uint8_t*, uint32_t, ImageKind, const ImageSize& decoded, TextureToken*)=nullptr;
    void (*release)(void*, TextureToken)=nullptr;
};
struct ImageView { ImageState state=ImageState::Missing; TextureToken texture{}; uint32_t generation=0; ImageSize size{}; };
// A cache keyed by URL. Images stay until the least recently used one has to
// make room, so scrolling back, changing section or reopening a mod shows them
// again without another request. reset() is the only call that drops them all.
class Images {
public:
    static constexpr uint32_t THUMBNAILS=160, PREVIEWS=8, SLOTS=THUMBNAILS+PREVIEWS;
    static constexpr uint32_t MAX_THUMBNAIL_ENCODED=512u<<10, MAX_ENCODED=2u<<20, MAX_INFLIGHT=3;
    // Small images (thumbnails and card pictures) share one pool and this budget.
    static constexpr uint32_t THUMBNAIL_BYTES=64u<<20, CARD_WIDTH=360;
    // A failed transfer is tried again twice, this many frames apart, before
    // the image is reported Failed. A rejected file is never fetched again.
    static constexpr uint32_t ATTEMPTS=3, RETRY_FRAMES=45;
    void attach(const P4M_HttpsApi*, const char* owner, const ImageBackend&);
    void reset(uint32_t generation);
    void begin_visible();
    // On screen this frame. Requests and uploads follow the order of these
    // calls, so ask from the top of the screen down.
    ImageView want(uint64_t submission, const char* url, ImageKind);
    // Likely to be on screen soon. Fetched after everything wanted.
    ImageView prefetch(uint64_t submission, const char* url, ImageKind);
    // What is already held for a URL. Asks for nothing: Missing unless the
    // image was requested before. Does not count as a use.
    ImageView cached(const char* url, ImageKind) const;
    void retry(uint64_t submission, ImageKind);
    void pump(); // at most one texture upload per frame
    uint32_t resident_bytes() const { return resident_bytes_; }
    uint32_t pending() const;
    uint32_t unsettled() const;
    uint32_t failed() const;
private:
    struct Slot {
        uint64_t submission=0, touched=0, retry_at=0;
        char url[P4M_HTTPS_URL_BYTES]{};
        ImageKind kind=ImageKind::Thumbnail;
        ImageState state=ImageState::Missing;
        TextureToken texture{};
        ImageSize size{};
        P4M_Transfer transfer=0;
        uint32_t bytes=0, order=0, attempts=0, hash=0;
        bool wanted=false, prefetch=false;
    };
    const P4M_HttpsApi* api_=nullptr;
    char owner_[P4M_ID_BYTES]{};
    ImageBackend backend_{};
    Slot slots_[SLOTS]{};
    uint32_t generation_=0, resident_bytes_=0, thumbnail_bytes_=0, sequence_=0;
    uint64_t clock_=0;
    alignas(16) uint8_t encoded_[MAX_ENCODED]{};
    void decoded(Slot&, bool ok, TextureToken, const ImageSize&);
    void drop(Slot&);
    Slot* acquire(uint64_t, const char*, ImageKind);
    void transfer_failed(Slot&, bool permanent);
    void trim_thumbnails();
    static uint32_t rank(const Slot& s) { return s.wanted?0:s.prefetch?1:2; }
    static bool before(const Slot& a, const Slot& b) { return rank(a)!=rank(b)?rank(a)<rank(b):a.order<b.order; }
};
}}
