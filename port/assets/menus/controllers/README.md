# Controller pictures

The pictures of the controllers drawn under the button-layout overlay
(Gamepad Setup's BUTTON LAYOUT, and the profile settings' picture of Gamepad
Setup): `tools/port_settings.py`'s `CONTROLLERS` says which platform draws
which, and where each button is on it.

The Switch's and the PS5's are on the Multiplayer screen's Internet and LAN
icons too (`port/assets/menus/menu_icons`).

They are AL2009man's [Gamepad Asset Pack](https://github.com/AL2009man/Gamepad-Asset-Pack)
(MIT, `Gamepad-Asset-Pack-LICENSE.txt`, which the releases carry), at commit
`a6d1113b`, with the consoles' logos taken out:

| File | The pack's | Taken out |
| --- | --- | --- |
| `xbox_series.svg` | `Xbox Series X Controller VSCView Black.svg` | the Xbox button (`g13092`) |
| `dualsense.svg` | `DualSense VSCView SVG Midnight Black.svg` | the PS button (`path1961-5`) |
| `switch_pro.svg` | `Switch Pro Controller VSCView.svg` | the Nintendo Switch logo (`path14321`) |

`tools/ce_menus.py` copies them to `port_svg/controllers/`, beside the
overlay's pictures that draw them (librsvg reads an image only from the
picture's folder), tinted blue as the Xbox's pictures of its controller are.
