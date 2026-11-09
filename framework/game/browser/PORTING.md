# Mods frontend port

This ports the previous framework's native downloader frontend and its
controller, provider, image scheduler, and JSON layout. The sidebar, continuous
card grid, detail gallery, author pages, and controller navigation are retained.
`layout.json` is the authored layout; the module build embeds it into a generated
header in its output folder.

This is internal Tsuchi code. Private P4M transport descriptors preserve the
ported frontend's boundary; bridge.cpp connects them to Tsuchi HTTPS and the
managed installer. The former runtime, patches, and activation system are not
linked. The title entry and keyboard use Tsuchi's UI services. A per-screen
native renderer retains ownership and input capture while the old widgets draw.

Provider merges GameBanana and public GitHub topic discovery into one list.
Records retain their source and original identity; synthetic UI IDs keep
repositories, authors, and 64-bit GitHub release asset IDs distinct. Partial
source failure preserves successful results. GameBanana category and author
views stay scoped; GitHub author views query the same topic for that author.

Native addresses and widget layouts are in game/table.yaml and explicitly absent
for unqualified builds. The frontend currently enables only on 1.1.1. Controller
navigation is integrated; the former desktop mouse adapter is not included.

The restored GameBanana grid, detail/gallery, scrolling, and Back have been
observed in Eden. Host tests exercise mixed-source pages, author scope, source
failure, cancellation, identity, navigation, and the managed installer boundary.
A full in-game successful installation and restart remains to be qualified.
