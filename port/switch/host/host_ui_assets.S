/*
HOST_UI_ASSETS.S

What the menus' screens are drawn with (host_ui.c), in the program itself so
nothing beside it on the card is needed: the title and body fonts
(port/assets/fonts, OFL) and the panel the screens and their rows are framed
in (port/switch/art/menu_panel.svg). The paths are from the repository's root,
where the build runs; tools/switch_build.py makes this file depend on them.
*/

	.section .rodata
	.balign 16

	.global host_ui_title_font
	.global host_ui_title_font_end
host_ui_title_font:
	.incbin "port/assets/fonts/OpenCE-Regular.ttf"
host_ui_title_font_end:

	.balign 16
	.global host_ui_body_font
	.global host_ui_body_font_end
host_ui_body_font:
	.incbin "port/assets/fonts/Overpass-750.ttf"
host_ui_body_font_end:

	.balign 16
	.global host_ui_panel_svg
	.global host_ui_panel_svg_end
host_ui_panel_svg:
	.incbin "port/switch/art/menu_panel.svg"
host_ui_panel_svg_end:

	.section .note.GNU-stack, "", %progbits
