# Doom: doomgeneric's files (its Makefile's list, less the X11 platform, plus mus2mid) and BMDS's
# platform layer (programs/doom), with bmds-cc. Copied into the build directory by build.sh and run
# there: make SRC=... PLATFORM=... ROOT=... CC=...

DOOM = $(SRC)/doomgeneric
DEFINES = -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND -DNORMALUNIX \
          -DLINUX -D_DEFAULT_SOURCE
# doomgeneric's own warnings are upstream's: not shown. Ours fail the build.
DOOM_CFLAGS = -std=gnu11 -O2 -w $(DEFINES)
PLATFORM_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter $(DEFINES) -I$(DOOM)

DOOM_OBJS = dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode \
            d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale \
            i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu \
            m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj \
            p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp \
            r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff \
            s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc \
            i_input i_video doomgeneric mus2mid
PLATFORM_OBJS = platform sound

OBJS = $(addsuffix .o,$(DOOM_OBJS)) $(addprefix bmds_,$(addsuffix .o,$(PLATFORM_OBJS))) icons.o

doom: $(OBJS)
	$(CC) -o $@ $(OBJS)

%.o: $(DOOM)/%.c
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Its icons, built into the program (devplan/gui.md §4j).
icons.S: $(PLATFORM)/icon.py
	python3 $(PLATFORM)/icon.py .
	python3 $(ROOT)/sdk/tools/bmds-icon-notes -o $@ doom icon-16.png icon-32.png

icons.o: icons.S
	$(CC) -c $< -o $@

bmds_%.o: $(PLATFORM)/%.c $(PLATFORM)/doom_bmds.h
	$(CC) $(PLATFORM_CFLAGS) -c $< -o $@
