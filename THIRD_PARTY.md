# Third-party components

`wristctl` itself is AGPL-3.0 (see `LICENSE`). The C++ GUI links the same
framework libraries the sibling workspace uses; their terms travel with them.

| Component | Where it will live | License |
|---|---|---|
| App_shell | `libs/firstparty/framework/App_shell` | **No LICENSE file upstream** — resolved before shipping |
| Vk_Canvas_Lb_LAW | `libs/firstparty/framework/Vk_Canvas_Lb_LAW` | AGPL-3.0 |
| vulkan_font_engine | nested in Vk_Canvas | see its repository |
| img_decode_kit | nested in Vk_Canvas | see its repository |
| archive_engine | `libs/firstparty/archive_engine` | AGPL-3.0 |
| UI font (`assets/fonts/ui/ui.otf`) | `assets/fonts` submodule | see the fonts repository |

The protocol spec and the generated codecs are original work derived from a
decompiled copy of the vendor app and an HCI capture of the owner's watch. No
Casio binary, firmware image, city table, or tide-harmonic asset is
redistributed.
