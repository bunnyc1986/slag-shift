#pragma bank 255

#include <gb/cgb.h>
#include <stdint.h>
#include <string.h>
#include "states/slagshift.h"
#include "states/slag_art.h"
#include "actor.h"
#include "input.h"
#include "game_time.h"
#include "music_manager.h"
#include "system.h"

BANKREF_EXTERN(song_banked_embers_Data)
BANKREF_EXTERN(song_rivet_run_Data)
BANKREF_EXTERN(song_last_bell_Data)
extern const hUGESong_t song_banked_embers_Data;
extern const hUGESong_t song_rivet_run_Data;
extern const hUGESong_t song_last_bell_Data;

#define WIDTH 10
#define HEIGHT 20
#define HIDDEN 2
#define LOCK_DELAY 24
#define CLEAR_FRAMES 12
#define LEVEL_LIMIT 15
#define AWARD_DURATION 78
#define AWARD_WIDTH 10
#define AWARD_HEIGHT 5

enum phase { TITLE, PICK_LEVEL, FALLING, CLEARING, PAUSED, OVER };
typedef struct { int8_t x, y; } point_t;
typedef struct { point_t cells[4]; } shape_t;

/* Each nibble is a row, with its leftmost square in bit 3. */
static const uint16_t shapes[7][4] = {
    {0x0F00,0x2222,0x00F0,0x4444}, /* I */
    {0x6600,0x6600,0x6600,0x6600}, /* O */
    {0x4E00,0x4640,0x0E40,0x4C40}, /* T */
    {0x6C00,0x4620,0x06C0,0x8C40}, /* S */
    {0xC600,0x2640,0x0C60,0x4C80}, /* Z */
    {0x8E00,0x6440,0x0E20,0x44C0}, /* J */
    {0x2E00,0x4460,0x0E80,0xC440}  /* L */
};

static const palette_color_t inks[] = {
    RGB(2,3,5), RGB(6,9,13), RGB(31,16,7), RGB(31,30,25),
    RGB(2,3,5), RGB(4,12,15), RGB(6,26,27), RGB(23,31,30),
    RGB(2,3,5), RGB(15,10,3), RGB(31,24,6), RGB(31,30,20),
    RGB(2,3,5), RGB(13,6,17), RGB(24,14,29), RGB(31,24,31),
    RGB(2,3,5), RGB(4,13,9), RGB(12,26,16), RGB(25,31,22),
    RGB(2,3,5), RGB(15,5,7), RGB(30,11,13), RGB(31,24,22),
    RGB(2,3,5), RGB(5,8,17), RGB(10,18,30), RGB(24,28,31),
    RGB(2,3,5), RGB(15,7,4), RGB(31,16,6), RGB(31,26,17)
};

static const uint8_t fall_period[LEVEL_LIMIT] = {48,42,37,32,28,24,20,17,14,11,9,7,6,5,4};
static const uint16_t clear_value[] = {0,100,300,500,800};
static const int8_t kick_x[] = {0,-1,1,-2,2,0};
static const int8_t kick_y[] = {0,0,0,0,0,-1};

static uint8_t board[HEIGHT][WIDTH], full[HEIGHT];
static uint8_t phase, resume_phase, piece, next_piece, rotation;
static uint8_t unlocked_level, selected_level, target_met;
static uint8_t endless;
static uint8_t award_ticks;
static const uint8_t *award_frames;
static uint8_t paused_track_bank;
static uint8_t bag[7], remaining, level, fall_clock, lock_clock, lock_resets;
static uint8_t repeat_dir, repeat_clock, clear_clock, cleared;
static uint16_t rng, lines;
static uint32_t score;
static shape_t shape;
static int8_t px, py, ghost_y;

/* GB Studio uses SRAM banks 0–2 for its save slots. Bank 3 holds one
 * independently committed stamp per unlock, so a partial new write cannot
 * invalidate levels already earned. */
#define PROGRESS_STAMPS ((volatile uint8_t *)0xA000)
static uint8_t read_progress(void) {
    uint8_t previous = _current_ram_bank, highest = 1;
    SWITCH_RAM_BANK(3, RAM_BANKS_ONLY);
    for (uint8_t n = 2; n <= LEVEL_LIMIT; n++) {
        uint8_t i = (n - 2) * 3;
        if (PROGRESS_STAMPS[i] != 'S' || PROGRESS_STAMPS[i+1] != 'L' ||
            PROGRESS_STAMPS[i+2] != (0xA0 | n)) break;
        highest = n;
    }
    SWITCH_RAM_BANK(previous, RAM_BANKS_ONLY);
    return highest;
}

static void save_unlock(uint8_t n) {
    uint8_t previous = _current_ram_bank, i = (n - 2) * 3;
    SWITCH_RAM_BANK(3, RAM_BANKS_ONLY);
    PROGRESS_STAMPS[i] = 'S';
    PROGRESS_STAMPS[i+1] = 'L';
    PROGRESS_STAMPS[i+2] = 0xA0 | n;
    SWITCH_RAM_BANK(previous, RAM_BANKS_ONLY);
    unlocked_level = n;
}

static uint16_t goal_for(uint8_t n) {
    return 500u * n;
}

static uint16_t random_word(void) {
    rng ^= rng << 7;
    rng ^= rng >> 9;
    rng ^= rng << 8;
    return rng;
}

static uint8_t take_piece(void) {
    if (!remaining) {
        for (uint8_t i = 0; i < 7; i++) bag[i] = i;
        remaining = 7;
    }
    uint8_t slot = random_word() % remaining;
    uint8_t value = bag[slot];
    bag[slot] = bag[--remaining];
    return value;
}

static void seed_board(void) {
    uint8_t gap = random_word() % WIDTH;
    uint8_t peak = (gap + 1 + random_word() % (WIDTH - 1)) % WIDTH;
    uint8_t depth = 2 + (level - 1) / 2;
    memset(board, 0, sizeof(board));
    for (uint8_t x = 0; x < WIDTH; x++) {
        if (x == gap) continue;
        uint8_t height = x == peak ? depth : 1 + random_word() % depth;
        for (uint8_t i = 0; i < height; i++) board[HEIGHT-1-i][x] = 1 + random_word() % 7;
    }
}

static void decode(shape_t *s, uint8_t kind, uint8_t orientation) {
    uint16_t bits = shapes[kind][orientation];
    uint8_t n = 0;
    for (int8_t y = 0; y < 4; y++) {
        for (int8_t x = 0; x < 4; x++, bits <<= 1) {
            if (bits & 0x8000) {
                s->cells[n].x = x;
                s->cells[n++].y = y;
            }
        }
    }
}

static uint8_t fits(const shape_t *s, int8_t x, int8_t y) {
    for (uint8_t i = 0; i < 4; i++) {
        int8_t col = x + s->cells[i].x, row = y + s->cells[i].y;
        if ((uint8_t)col >= WIDTH || (uint8_t)row >= HEIGHT || board[row][col]) return 0;
    }
    return 1;
}

static void cell(int8_t x, int8_t y, uint8_t tile, uint8_t palette) {
    if (y < HIDDEN) return;
    set_bkg_tile_xy(x + 1, y - HIDDEN, tile);
    set_bkg_attribute_xy(x + 1, y - HIDDEN, palette);
}

static void paint_shape(int8_t y, uint8_t tile, uint8_t palette) {
    for (uint8_t i = 0; i < 4; i++) cell(px + shape.cells[i].x, y + shape.cells[i].y, tile, palette);
}

static void erase_piece(void) {
    paint_shape(ghost_y, BED, 0);
    paint_shape(py, BED, 0);
}

static void draw_piece(void) {
    ghost_y = py;
    while (fits(&shape, px, ghost_y + 1)) ghost_y++;
    paint_shape(ghost_y, GUIDE, 0);
    paint_shape(py, METAL, piece + 1);
}

static void render_board(void) {
    uint8_t row_tiles[WIDTH];
    for (uint8_t y = HIDDEN; y < HEIGHT; y++) {
        for (uint8_t x = 0; x < WIDTH; x++) row_tiles[x] = board[y][x] ? METAL : BED;
        set_bkg_tiles(1, y - HIDDEN, WIDTH, 1, row_tiles);
        set_bkg_attributes(1, y - HIDDEN, WIDTH, 1, board[y]);
    }
}

static void tone(uint8_t length, uint8_t pitch, uint8_t octave) {
    NR21_REG = 0x80u | length;
    NR22_REG = 0x92;
    NR23_REG = pitch;
    NR24_REG = 0xC0u | octave;
}

static void play_clear_note(uint8_t step) {
    /* Pulse 2 climbs a D-minor arpeggio. Each extra row starts one note higher. */
    static const uint16_t pitches[] = {1602,1673,1750,1825,1860,1899,1936};
    uint16_t pitch = pitches[cleared - 1 + step];
    NR21_REG = step == 3 ? 0x9C : 0x72;
    NR22_REG = step == 3 ? 0xA2 : 0xB1;
    NR23_REG = (uint8_t)pitch;
    NR24_REG = 0xC0 | (uint8_t)(pitch >> 8);
}

static void number(uint8_t x, uint8_t y, uint8_t width, uint32_t value) {
    uint8_t data[6];
    for (uint8_t i = width; i; i--) {
        data[i-1] = glyphs[value % 10];
        value /= 10;
    }
    set_bkg_tiles(x, y, width, 1, data);
}

static void draw_score(void) {
    number(12, 9, 6, score);
}

static void meter(void) {
    draw_score();
    number(12, 12, 4, lines);
    if (!endless) number(12, 15, 2, level);
}

static void preview(void) {
    shape_t upcoming;
    decode(&upcoming, next_piece, 0);
    fill_bkg_rect(14, 4, 4, 3, BLANK);
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t x = 14 + upcoming.cells[i].x, y = 5 + upcoming.cells[i].y;
        set_bkg_tile_xy(x, y, METAL);
        set_bkg_attribute_xy(x, y, next_piece + 1);
    }
}

static void text_row(uint8_t x, uint8_t y, const char *s) {
    while (*s) {
        set_bkg_tile_xy(x++, y, *s == ' ' ? BLANK : glyphs[10 + *s - 'A']);
        s++;
    }
}

static void draw_goal(uint8_t new_unlock) {
    fill_bkg_rect(12,0,8,2,BLANK);
    if (endless) {
        text_row(12,0,"ENDLESS");
    } else if (level == LEVEL_LIMIT) {
        text_row(12,0,"FINAL"); text_row(12,1,"STAGE");
    } else if (new_unlock) {
        text_row(12,0,"NEXT LV"); text_row(12,1,"UNLOCKED");
    } else if (target_met) {
        text_row(12,0,"GOAL"); text_row(12,1,"MET");
    } else {
        text_row(12,0,"GOAL"); number(12,1,6,goal_for(level));
    }
}

static void check_goal(void) {
    if (!endless && !target_met && level < LEVEL_LIMIT && score >= goal_for(level)) {
        uint8_t new_unlock = level == unlocked_level;
        target_met = 1;
        if (new_unlock) save_unlock(level+1);
        draw_goal(new_unlock);
        award_frames = new_unlock ? unlock_frames : goal_frames;
        award_ticks = AWARD_DURATION;
    }
}

static void draw_choices(void) {
    fill_bkg_rect(12,3,7,1,BLANK);
    if (endless) text_row(12,3,"ENDLESS");
    else number(12,3,2,selected_level);
    for (uint8_t i = 0; i < LEVEL_LIMIT; i++) {
        uint8_t x = 1 + (i % 5) * 4, y = 5 + (i / 5) * 2, n = i + 1;
        uint8_t attr = !endless && n == selected_level ? 1 : n > unlocked_level ? 2 : 0;
        number(x,y,2,n);
        set_bkg_attribute_xy(x,y,attr);
        set_bkg_attribute_xy(x+1,y,attr);
    }
    text_row(1,11,"ENDLESS");
    VBK_REG = 1;
    fill_bkg_rect(1,11,7,1,endless ? 1 : 0);
    VBK_REG = 0;
    if (endless || selected_level == LEVEL_LIMIT) text_row(12,13,"NONE");
    else number(12,13,4,goal_for(selected_level));
}

static void show_level_picker(void) {
    static const palette_color_t menu_colors[] = {
        RGB(31,16,7), RGB(31,16,7), RGB(31,16,7), RGB(2,3,5),
        RGB(2,3,5), RGB(6,9,13), RGB(6,9,13), RGB(11,13,16)
    };
    phase = PICK_LEVEL;
    award_ticks = 0;
    selected_level = unlocked_level;
    VBK_REG = 0;
    set_bkg_data(0,PICKER_COUNT,picker_pixels);
    set_bkg_palette(0,1,inks);
    set_bkg_palette(1,2,menu_colors);
    set_bkg_tiles(0,0,20,18,picker_map);
    VBK_REG = 1;
    fill_bkg_rect(0,0,20,18,0);
    VBK_REG = 0;
    draw_choices();
    music_load(BANK(song_banked_embers_Data), &song_banked_embers_Data);
}

static void message(const char *heading, uint8_t count, const char *action, uint8_t action_len) {
    uint8_t attrs[WIDTH] = {0};
    for (uint8_t y = 6; y < 11; y++) {
        fill_bkg_rect(1,y,WIDTH,1,(y==6 || y==10) ? GUIDE : BLANK);
        set_bkg_attributes(1,y,WIDTH,1,attrs);
    }
    text_row(1+(WIDTH-count)/2,7,heading);
    text_row(1+(WIDTH-action_len)/2,9,action);
}

static void begin_board(void);

/* Finish any line clear before the plaque, then deal the next opening. */
static void advance_award(void) {
    uint8_t elapsed = AWARD_DURATION - award_ticks;
    if (elapsed == 0) {
        VBK_REG = 1;
        fill_bkg_rect(1,6,AWARD_WIDTH,AWARD_HEIGHT,8);
        VBK_REG = 0;
    }
    if (elapsed <= 40 && (elapsed & 7) == 0) {
        uint8_t frame = elapsed >> 3;
        set_bkg_tiles(1,6,AWARD_WIDTH,AWARD_HEIGHT,
                      award_frames + frame * (AWARD_WIDTH * AWARD_HEIGHT));
    }
    if (--award_ticks == 0) {
        level++;
        begin_board();
    }
}

static void spawn(void) {
    piece = next_piece;
    next_piece = take_piece();
    rotation = 0; px = 3; py = HIDDEN;
    fall_clock = lock_clock = lock_resets = 0;
    decode(&shape, piece, rotation);
    preview();
    if (!fits(&shape, px, py)) {
        phase = OVER;
        music_load(BANK(song_last_bell_Data), &song_last_bell_Data);
        message("GAMEOVER",8,"RESTART",7);
    } else {
        phase = FALLING;
        draw_piece();
    }
}

static void begin_board(void) {
    target_met = 0;
    repeat_dir = 0;
    score = 0;
    if (endless) memset(board, 0, sizeof(board));
    else seed_board();
    render_board();
    meter();
    draw_goal(FALSE);
    spawn();
}

static void start_game(void) {
    rng ^= ((uint16_t)DIV_REG << 8) | game_time;
    if (!rng) rng = 0x6D31;
    remaining = 0;
    lines = 0;
    level = endless ? 1 : selected_level;
    VBK_REG = 0;
    set_bkg_data(0, ART_COUNT, tile_pixels);
    set_bkg_palette(0, 8, inks);
    set_bkg_tiles(0,0,20,18,panel_map);
    VBK_REG = 1;
    set_bkg_data(0, AWARD_TILE_COUNT, award_pixels);
    fill_bkg_rect(0,0,20,18,0);
    VBK_REG = 0;
    if (endless) fill_bkg_rect(12,14,8,3,BLANK);
    next_piece = take_piece();
    begin_board();
    music_load(BANK(song_rivet_run_Data), &song_rivet_run_Data);
}

static void settle(void) {
    for (uint8_t i = 0; i < 4; i++) board[py + shape.cells[i].y][px + shape.cells[i].x] = piece + 1;
    paint_shape(py, METAL, piece + 1);
    cleared = 0;
    for (uint8_t y = 0; y < HEIGHT; y++) {
        uint8_t x = 0;
        while (x < WIDTH && board[y][x]) x++;
        full[y] = x == WIDTH;
        if (full[y]) {
            cleared++;
            if (y >= HIDDEN) fill_bkg_rect(1, y-HIDDEN, WIDTH, 1, FLASH);
        }
    }
    if (cleared) {
        clear_clock = CLEAR_FRAMES;
        phase = CLEARING;
        play_clear_note(0);
    } else {
        tone(52,0x30,5);
        if (!target_met) spawn();
    }
    meter();
}

static void finish_clear(void) {
    int8_t dest = HEIGHT-1;
    for (int8_t y = HEIGHT-1; y >= 0; y--) {
        if (!full[y]) {
            if (dest != y) memcpy(board[dest],board[y],WIDTH);
            dest--;
        }
    }
    while (dest >= 0) memset(board[dest--],0,WIDTH);
    score += (uint32_t)clear_value[cleared] * level;
    check_goal();
    lines += cleared;
    render_board();
    meter();
    phase = FALLING;
    if (!target_met) spawn();
}

static uint8_t horizontal(void) {
    uint8_t dir = joy & (J_LEFT|J_RIGHT);
    if (!dir || dir == (J_LEFT|J_RIGHT)) { repeat_dir = 0; return 0; }
    if (dir != repeat_dir) { repeat_dir = dir; repeat_clock = 11; return dir; }
    if (--repeat_clock) return 0;
    repeat_clock = 3;
    return dir;
}

static void update_falling(void) {
    int8_t x = px, y = py;
    uint8_t orientation = rotation, moved = 0;
    shape_t candidate = shape;
    if (INPUT_A_PRESSED || INPUT_B_PRESSED) {
        uint8_t turn = (rotation + (INPUT_A_PRESSED ? 1 : 3)) & 3;
        shape_t rotated;
        decode(&rotated,piece,turn);
        for (uint8_t i = 0; i < sizeof(kick_x); i++) {
            if (fits(&rotated,x+kick_x[i],y+kick_y[i])) {
                candidate = rotated; orientation = turn;
                x += kick_x[i]; y += kick_y[i];
                moved = 1;
                tone(59,0x50,7);
                break;
            }
        }
    }
    uint8_t dir = horizontal();
    if (dir) {
        int8_t shift = dir == J_RIGHT ? 1 : -1;
        if (fits(&candidate,x+shift,y)) { x += shift; moved = 1; }
    }
    if (moved && lock_clock && lock_resets < 8) { lock_clock = 0; lock_resets++; }
    uint8_t hard = INPUT_UP_PRESSED;
    if (hard) {
        while (fits(&candidate,x,y+1)) { y++; score += 2; }
        check_goal();
    } else if (++fall_clock >= (INPUT_DOWN ? 2 : fall_period[level-1])) {
        fall_clock = 0;
        if (fits(&candidate,x,y+1)) {
            y++; lock_clock = 0;
            if (INPUT_DOWN) { score++; check_goal(); draw_score(); }
        }
    }
    if (x != px || y != py || orientation != rotation) {
        erase_piece();
        shape = candidate; px = x; py = y; rotation = orientation;
        draw_piece();
    }
    if (hard || (!fits(&shape,px,py+1) && ++lock_clock >= LOCK_DELAY)) settle();
}

void slagshift_init(void) BANKED {
    PLAYER.flags |= ACTOR_FLAG_HIDDEN;
    phase = TITLE;
    endless = FALSE;
    unlocked_level = read_progress();
    NR52_REG = 0x80;
    NR50_REG = 0x77;
    NR51_REG |= 0x22;
    music_load(BANK(song_banked_embers_Data), &song_banked_embers_Data);
}

void slagshift_update(void) BANKED {
    if (award_ticks && phase == FALLING) {
        uint8_t interrupt = INPUT_START_PRESSED;
        if (interrupt) award_ticks = 1;
        advance_award();
        if (!interrupt) return;
    }
    if (phase == TITLE || phase == OVER) {
        if (INPUT_START_PRESSED || INPUT_A_PRESSED) show_level_picker();
        return;
    }
    if (phase == PICK_LEVEL) {
        if (INPUT_UP_PRESSED || INPUT_DOWN_PRESSED) {
            endless = !endless;
            draw_choices();
        } else if (!endless && INPUT_LEFT_PRESSED && selected_level > 1) {
            selected_level--; draw_choices();
        } else if (!endless && INPUT_RIGHT_PRESSED && selected_level < unlocked_level) {
            selected_level++; draw_choices();
        }
        if (INPUT_START_PRESSED || INPUT_A_PRESSED) start_game();
        return;
    }
    if (phase == PAUSED && INPUT_SELECT_PRESSED) {
        show_level_picker();
        return;
    }
    if (INPUT_START_PRESSED) {
        if (phase == PAUSED) {
            phase = resume_phase;
            music_current_track_bank = paused_track_bank;
            render_board();
            if (phase == FALLING) draw_piece();
            else for (uint8_t y=HIDDEN; y<HEIGHT; y++)
                if (full[y]) fill_bkg_rect(1,y-HIDDEN,WIDTH,1,FLASH);
        } else {
            resume_phase = phase; phase = PAUSED;
            /* Suspend the timer-driven score without rewinding its pattern. */
            paused_track_bank = music_current_track_bank;
            music_stop();
            message("PAUSED",6,"START",5);
            text_row(1,8,"SEL LEVELS");
        }
        return;
    }
    if (phase == FALLING) update_falling();
    else if (phase == CLEARING) {
        clear_clock--;
        if (!clear_clock) finish_clear();
        else if (clear_clock % 3 == 0) play_clear_note((CLEAR_FRAMES - clear_clock) / 3);
    }
}
