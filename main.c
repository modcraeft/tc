#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#if 0
#define WW 2560
#define WH 1440
#define WX 0
#define WY 0
#else
#define WW 751
#define WH 822
#define WX 1794
#define WY 850
#endif

#define DELAY 10

#define DEBUG false

#define MAX_ENTRIES 50
#define MARGIN_X 7
#define MARGIN_Y 5
#define LINE_HEIGHT 20
#define MAX_WRAP_WIDTH (WW - 2 * MARGIN_X)
#define SPACE_ADVANCE 7

float RATE = 20;
float RATE_RESET = 20;

// Globals for window size.
int screen_width = WW;
int screen_height = WH;

typedef struct {
    uint8_t r, g, b, a;
} Color;

Color c1 = {0x55, 0x99, 0xFF, 0xFF};
Color c2 = {0x77, 0x77, 0x77, 0xFF};

typedef struct {
    int x, y;
    uint8_t r, g, b, a;
} Pixel;

typedef struct {
    int width, height;
    int num_pixels;
    Pixel* pixels;
    int advance;
} GMap;

typedef struct {
    char* original_line;
    char** wrapped_lines;
    int num_wrapped;
    int rendered_height;
} ChatEntry;

// Globals.
SDL_Window* w = NULL;
SDL_Renderer* r = NULL;
TTF_Font* font = NULL;
GMap glyphs[128];
int loaded_glyphs = 0;
bool params = false;

// Chat log globals.
ChatEntry** chat_log = NULL;
int chat_log_size = 0;
int chat_log_capacity = 0;
FILE* log_file = NULL;
off_t last_file_pos = 0;
time_t last_mod_time = 0;

void render_gmap(SDL_Renderer* r, int ch, int x, int y, bool colon_flag);

int get_advance(int ch_code) {
    if (ch_code == 32) {  // Space.
        return SPACE_ADVANCE;
    }
    if (ch_code >= 32 && ch_code < 127 && glyphs[ch_code].advance > 0) {
        return glyphs[ch_code].advance;
    }
    return 20;  // Fallback.
}

void wrap_text(const char* text, int max_width_pixels, char*** wrapped_lines_out, int* num_wrapped_out) {
    *wrapped_lines_out = NULL;
    *num_wrapped_out = 0;

    if (!text || strlen(text) == 0) return;

    // Split into words
    size_t text_len = strlen(text);
    char* line_copy = (char*)malloc(text_len + 1);  // Modifiable copy.
    if (!line_copy) return;
    strcpy(line_copy, text);
    char* word = strtok(line_copy, " \t\n");
    if (!word) {
        free(line_copy);
        return;
    }

    char* current_line = (char*)malloc(1024);  // Temp buffer
    if (!current_line) {
        free(line_copy);
        return;
    }
    memset(current_line, 0, 1024);
    int current_line_len = 0;
    int current_advance = 0;

    do {
        int word_len = strlen(word);
        int word_advance = 0;
        for (int i = 0; i < word_len; i++) {
            int ch_code = (int)word[i];
            word_advance += get_advance(ch_code);
        }

        // Check if adding word exceeds width.
        int space_advance = (current_line_len > 0) ? SPACE_ADVANCE : 0;
        if (current_advance + space_advance + word_advance > max_width_pixels && current_line_len > 0) {
            // Push current line.
            current_line[current_line_len] = '\0';
            *wrapped_lines_out = (char**)realloc(*wrapped_lines_out, (*num_wrapped_out + 1) * sizeof(char*));
            if (*wrapped_lines_out) {
                char* new_line = (char*)calloc(1, current_line_len + 1);
                if (new_line) {
                    strcpy(new_line, current_line);
                    (*wrapped_lines_out)[*num_wrapped_out] = new_line;
                }
                (*num_wrapped_out)++;
            }

            // Reset for new line
            memset(current_line, 0, 1024);
            strcpy(current_line, word);
            current_line_len = word_len;
            current_advance = word_advance;
        } else {
            // Append to current
            if (current_line_len > 0) {
                current_line[current_line_len] = ' ';
                current_line_len++;
                current_advance += SPACE_ADVANCE;  // Use fixed space advance.
            }
            strcat(current_line, word);
            current_line_len += word_len;
            current_advance += word_advance;
        }
    } while ((word = strtok(NULL, " \t\n")) != NULL);

    // Push final line
    if (current_line_len > 0) {
        current_line[current_line_len] = '\0';
        *wrapped_lines_out = (char**)realloc(*wrapped_lines_out, (*num_wrapped_out + 1) * sizeof(char*));
        if (*wrapped_lines_out) {
            char* new_line = (char*)calloc(1, current_line_len + 1);  // Zeroed alloc.
            if (new_line) {
                strcpy(new_line, current_line);
                (*wrapped_lines_out)[*num_wrapped_out] = new_line;
            }
            (*num_wrapped_out)++;
        }
    }

    free(current_line);
    free(line_copy);
}

void add_chat_entry(const char* line) {
    if (!line || strlen(line) == 0) return;

    ChatEntry* entry = (ChatEntry*)calloc(1, sizeof(ChatEntry));
    if (!entry) return;

    size_t line_len = strlen(line);
    entry->original_line = (char*)malloc(line_len + 1);
    if (!entry->original_line) {
        free(entry);
        return;
    }
    strcpy(entry->original_line, line);

    wrap_text(line, MAX_WRAP_WIDTH, &entry->wrapped_lines, &entry->num_wrapped);
    entry->rendered_height = entry->num_wrapped * LINE_HEIGHT;

    if (chat_log_size >= chat_log_capacity) {
        chat_log_capacity = chat_log_capacity == 0 ? 10 : chat_log_capacity * 2;
        chat_log = (ChatEntry**)realloc(chat_log, chat_log_capacity * sizeof(ChatEntry*));
        if (!chat_log) {
            // Cleanup partial
            free(entry->original_line);
            free(entry);
            return;
        }
    }
    chat_log[chat_log_size] = entry;
    chat_log_size++;

    // Evict oldest if over limit
    if (chat_log_size > MAX_ENTRIES) {
        ChatEntry* old = chat_log[0];
        for (int i = 0; i < old->num_wrapped; i++) {
            free(old->wrapped_lines[i]);
        }
        free(old->wrapped_lines);
        free(old->original_line);
        free(old);
        // Shift array
        for (int i = 0; i < chat_log_size - 1; i++) {
            chat_log[i] = chat_log[i + 1];
        }
        chat_log_size--;
    }

    if (DEBUG) printf("Added entry %d: '%s' (wrapped to %d lines)\n", chat_log_size, line, entry->num_wrapped);
}

int render_chat_entry(SDL_Renderer* r, ChatEntry* entry, int x_start, int y_start) {
    int current_y = y_start;
    for (int line_idx = 0; line_idx < entry->num_wrapped; line_idx++) {
        const char* line_text = entry->wrapped_lines[line_idx];
        if (!line_text) continue;  // Safety.
        int current_x = x_start;

        bool colon_flag = false;
        // Render each char in the wrapped line
        size_t text_len = strlen(line_text);
        for (size_t i = 0; i < text_len; i++) {
            int c = (int)line_text[i];
            if (c >= 32 && c < 127) {
                if (c == 32) { //Space
                    current_x += SPACE_ADVANCE;
                } else if (glyphs[c].num_pixels > 0) {
                    render_gmap(r, c, current_x, current_y, colon_flag);
                   //current_x += glyphs[c].advance;
                    // Advance x basic kerning approx
                    current_x += glyphs[c].width + (glyphs[c].advance - glyphs[c].width) / 2;
                } else {
                    current_x += get_advance(c);  // Fallback advance without rendering.
                }
                if(c == ':' || line_idx > 0 || c == ' ') colon_flag = true;
            }
        }

        current_y += LINE_HEIGHT;
    }
    return current_y;
}

void poll_log_file(const char* filepath) {
    struct stat file_stat;
    if (stat(filepath, &file_stat) != 0) {
        if (DEBUG) fprintf(stderr, "Stat failed for %s: %s\n", filepath, strerror(errno));
        return;
    }

    if (file_stat.st_mtime <= last_mod_time && file_stat.st_size <= last_file_pos) {
        return;  // No change
    }
    else {
        RATE = RATE_RESET;
    }

    // File changed/grown: Open and read new lines
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        if (DEBUG) fprintf(stderr, "Failed to open %s: %s\n", filepath, strerror(errno));
        return;
    }

    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    off_t read_pos = last_file_pos;
    fseek(fp, read_pos, SEEK_SET);
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // Trim newline
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') buffer[len - 1] = '\0';
        add_chat_entry(buffer);

        // Re-zero
        memset(buffer, 0, sizeof(buffer));
        read_pos = ftell(fp);
    }

    last_file_pos = ftell(fp);
    last_mod_time = file_stat.st_mtime;
    fclose(fp);

    if (DEBUG) printf("Polled %s: Added %ld new bytes (total entries: %d)\n", filepath, read_pos - last_file_pos, chat_log_size);
}

int load_font(char *path, int size)
{
    font = TTF_OpenFont(path, size);
    if (!font) {
        fprintf(stderr, "Font load failed: %s (check path: '%s')\n", TTF_GetError(), path);
        return 1;
    }
    if(DEBUG) printf("Font loaded: %s, size %dpt (anti-aliased).\n", TTF_FontFaceFamilyName(font), size);

    // Pre-load glyphs into GMap array (ASCII 32-126)
    SDL_Color fg_color = {255, 255, 255, 255};  // White foreground
    loaded_glyphs = 0;
    int empty_count = 0;
    uint8_t threshold = 50;  // Extract pixels with a > threshold (AA edges)

    for (int c = 32; c < 127; c++) {
        // Render blended (AA)
        SDL_Surface* surface = TTF_RenderGlyph_Blended(font, (Uint16)c, fg_color);
        bool use_blended = true;

        // Fallback to solid if empty
        if (!surface || surface->w == 0 || surface->h == 0) {
            if (surface) {
                SDL_FreeSurface(surface);
                if (DEBUG && empty_count < 5) printf("Blended empty for '%c' (trying solid)\n", c);
            } else {
                if (DEBUG && empty_count < 5) printf("Blended NULL for '%c': %s (trying solid)\n", c, TTF_GetError());
            }
            surface = TTF_RenderGlyph_Solid(font, (Uint16)c, fg_color);
            use_blended = false;
        }

        if (!surface || surface->w == 0 || surface->h == 0) {
            int surf_w = surface ? surface->w : 0;
            int surf_h = surface ? surface->h : 0;
            if (surface) SDL_FreeSurface(surface);
            empty_count++;
            glyphs[c].width = 0;
            glyphs[c].height = 0;
            glyphs[c].num_pixels = 0;
            glyphs[c].pixels = NULL;
            glyphs[c].advance = 0;
            if (DEBUG && (c == 'A' || empty_count < 3)) {
                printf("Failed for '%c': w=%d h=%d, error: %s\n", c, surf_w, surf_h, TTF_GetError());
            }
            continue;
        }

        // Extract visible pixels from surface
        int width = surface->w;
        int height = surface->h;
        int visible_count = 0;

        // First pass: Count visible pixels
        SDL_PixelFormat* s_format = SDL_AllocFormat(surface->format->format);
        if (!s_format) {
            fprintf(stderr, "Format alloc failed for '%c'\n", c);
            SDL_FreeSurface(surface);
            continue;
        }
        uint8_t r_temp, g_temp, b_temp, a_temp;
        for (int j = 0; j < height; ++j) {
            Uint8* row = (Uint8*)surface->pixels + j * surface->pitch;
            for (int i = 0; i < width; ++i) {
                Uint32 pixel = *((Uint32*)(row + i * 4));  // Assume 4 bpp.
                SDL_GetRGBA(pixel, s_format, &r_temp, &g_temp, &b_temp, &a_temp);
                if (a_temp > threshold) visible_count++;
            }
        }
        SDL_FreeFormat(s_format);

        if (visible_count == 0) {
            SDL_FreeSurface(surface);
            empty_count++;
            glyphs[c].width = width;
            glyphs[c].height = height;
            glyphs[c].num_pixels = 0;
            glyphs[c].pixels = NULL;
            glyphs[c].advance = 0;
            continue;
        }

        // Second pass: Alloc and store visible pixels
        glyphs[c].pixels = (Pixel*)calloc(visible_count, sizeof(Pixel));  // Zeroed alloc.
        if (!glyphs[c].pixels) {
            fprintf(stderr, "Memory alloc failed for '%c' pixels (%d)\n", c, visible_count);
            SDL_FreeSurface(surface);
            continue;
        }
        s_format = SDL_AllocFormat(surface->format->format);  // Re-alloc for extraction
        int stored_count = 0;
        for (int j = 0; j < height; ++j) {
            Uint8* row = (Uint8*)surface->pixels + j * surface->pitch;
            for (int i = 0; i < width; ++i) {
                Uint32 pixel = *((Uint32*)(row + i * 4));
                SDL_GetRGBA(pixel, s_format, &r_temp, &g_temp, &b_temp, &a_temp);
                if (a_temp > threshold) {
                    glyphs[c].pixels[stored_count].x = i;
                    glyphs[c].pixels[stored_count].y = j;
                    glyphs[c].pixels[stored_count].r = r_temp;
                    glyphs[c].pixels[stored_count].g = g_temp;
                    glyphs[c].pixels[stored_count].b = b_temp;
                    glyphs[c].pixels[stored_count].a = a_temp;
                    stored_count++;
                }
            }
        }
        SDL_FreeFormat(s_format);
        SDL_FreeSurface(surface);

        // Store metadata
        glyphs[c].width = width;
        glyphs[c].height = height;
        glyphs[c].num_pixels = stored_count;
        TTF_GlyphMetrics(font, (Uint16)c, NULL, NULL, NULL, &glyphs[c].advance, NULL);
        loaded_glyphs++;

        // Debug samples
        if (c == 'H' || c == 'e' || c == 'l' || c == 'o' || c == ' ' || c == 'A') {
            if(DEBUG) printf("Glyph '%c': w=%d, h=%d, visible_pixels=%d, advance=%d (%s)\n", 
                             c, glyphs[c].width, glyphs[c].height, stored_count, glyphs[c].advance, use_blended ? "AA blended" : "solid");
        }
    }

    if(DEBUG) {
        printf("Loaded %d glyphs into GMap array (%d empty). Sample 'A' (65): pixels=%d\n", 
               loaded_glyphs, empty_count, glyphs[65].num_pixels);
        if (empty_count > 90) printf("CRITICAL: Most glyphs empty—check font path.\n");
    }


    // Free
    TTF_CloseFont(font);
    font = NULL;

    return 0;
}

void render_gmap(SDL_Renderer* r, int ch, int x, int y, bool colon_flag) {
    if (ch < 0 || ch >= 128 || glyphs[ch].num_pixels == 0 || !glyphs[ch].pixels) {
        if (DEBUG) printf("Skipping invalid glyph '%c' (code %d)\n", (char)ch, ch);
        return;
    }

    if(RATE > 1.1) RATE -= 0.01; 

    int drawn_points = 0;
    for (int i = 0; i < glyphs[ch].num_pixels; ++i) {
        Pixel* p = &glyphs[ch].pixels[i];
        // Set color with alpha (blends on black bg)
        if(colon_flag == false) {
            SDL_SetRenderDrawColor(r, c1.r, c1.g, c1.b, p->a);
            //SDL_SetRenderDrawColor(r, 0x55, 0x99, 0xFF, 0xFF);
        }
        else {
            //SDL_SetRenderDrawColor(r, p->r, p->g, p->b, p->a);
            SDL_SetRenderDrawColor(r, c2.r, c2.g, c2.b, p->a);
            //SDL_SetRenderDrawColor(r, 0x77, 0x77, 0x77, 0xFF);
        }

        //Rand Offset
        int off_x = rand() % (int)RATE - (RATE/2);
        int off_y = rand() % (int)RATE - (RATE/2);

        //Random Noise
        //if(rand() % 100 == 1) SDL_SetRenderDrawColor(r, rand() % 255, rand() % 255, rand() % 255, p->a);
        //Regular Noise
        //if(i % 5 == 0) SDL_SetRenderDrawColor(r, 0x00, 0x00, 0x00, p->a);


        if (SDL_RenderDrawPoint(r, x + p->x + off_x, y + p->y + off_y) == 0) {
            drawn_points++;
        } else {
            if (DEBUG) fprintf(stderr, "DrawPoint failed for '%c' at (%d,%d): %s\n", (char)ch, x + p->x, y + p->y, SDL_GetError());
        }
    }

    if (DEBUG) printf("Rendered glyph '%c': %d points drawn\n", (char)ch, drawn_points);
}

int get_total_chat_height() {
    int total = 0;
    for (int i = 0; i < chat_log_size; i++) {
        if (chat_log[i]) {
            total += chat_log[i]->rendered_height;
        }
    }
    return total;
}

int main(int argc, char* argv[]) 
{ 
    if(argc >= 2) params = true;
    char *font_path;
    int font_size = 16;
    if(params == false) font_path = "fonts/Hack-Regular.ttf";
    else font_path = argv[1];
    if(argc >= 3) font_size = atoi(argv[2]);
    const char* log_filepath = (argc >= 4) ? argv[3] : "log.txt";

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL init failed: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() < 0) { fprintf(stderr, "TTF init failed: %s\n", TTF_GetError()); SDL_Quit(); return 1; }

    w = SDL_CreateWindow("tc", WX, WY, WW, WH, SDL_WINDOW_BORDERLESS);
    if (!w) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) {
        fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(w);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    if(load_font(font_path, font_size) != 0) {
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(w);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    // Force space advance if zero
    if (glyphs[32].advance == 0) {
        glyphs[32].advance = SPACE_ADVANCE;
        if (DEBUG) printf("Forced space advance to %d (was 0)\n", SPACE_ADVANCE);
    }

    // Initialize log file polling.
    log_file = fopen(log_filepath, "r");
    if (log_file) {
        fseek(log_file, 0, SEEK_END);
        last_file_pos = ftell(log_file);
        struct stat st;
        stat(log_filepath, &st);
        last_mod_time = st.st_mtime;
        if (DEBUG) printf("Initialized log file '%s' at position %ld\n", log_filepath, last_file_pos);
    } else {
        fprintf(stderr, "Warning: Could not open log file '%s'—create it with chat lines.\n", log_filepath);
    }



    int view_y_offset = 0;  // For scrolling

    // Initial load: Read existing lines
    if (log_file) {
        rewind(log_file);
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), log_file) != NULL) {
            size_t len = strlen(buffer);
            if (len > 0 && buffer[len - 1] == '\n') buffer[len - 1] = '\0';
            add_chat_entry(buffer);
        }
        fseek(log_file, 0, SEEK_END);
        last_file_pos = ftell(log_file);


        // Initial auto-scroll to bottom
        int total_height = get_total_chat_height();
        int visible_height = screen_height - 2 * MARGIN_Y;
        int max_offset = (total_height > visible_height) ? total_height - visible_height : 0;
        view_y_offset = max_offset;

    }

    int quit = false;
    SDL_Event e;

    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                quit = true;
            }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(w, &screen_width, &screen_height);

                // Re-clamp offset after resize
                int total_height = get_total_chat_height();
                int visible_height = screen_height - 2 * MARGIN_Y;
                int max_offset = (total_height > visible_height) ? total_height - visible_height : 0;
                if (view_y_offset > max_offset) {
                    view_y_offset = max_offset;
                }

            }
            // Simple scroll: Arrow keys (up/down for offset)
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_UP) view_y_offset -= 50;
                if (e.key.keysym.sym == SDLK_DOWN) view_y_offset += 50;

                // Clamp offset.
                int total_height = get_total_chat_height();
                int visible_height = screen_height - 2 * MARGIN_Y;
                int max_offset = (total_height > visible_height) ? total_height - visible_height : 0;
                view_y_offset = (view_y_offset < 0) ? 0 : (view_y_offset > max_offset ? max_offset : view_y_offset);

                if(e.key.keysym.sym == SDLK_e) RATE += RATE_RESET;
                if(e.key.keysym.sym == SDLK_F1) { c2.r = rand() % 255; c2.g = rand() % 255; c2.b = rand() % 255; }
                if(e.key.keysym.sym == SDLK_F2) { c1.r = rand() % 255; c1.g = rand() % 255; c1.b = rand() % 255; }
                if(e.key.keysym.sym == SDLK_F3) { c2.r = 0x77; c2.g = 0x77; c2.b = 0x77; }
                if(e.key.keysym.sym == SDLK_F4) { c1.r = 0x55; c1.g = 0x99; c1.b = 0xFF; }

            }
        }

        // Poll for new log entr
        if (log_file) {
            poll_log_file(log_filepath);
        }


        // Auto-scroll to bottom after polling
        int total_height = get_total_chat_height();
        int visible_height = screen_height - 2 * MARGIN_Y;
        int max_offset = (total_height > visible_height) ? total_height - visible_height : 0;
        view_y_offset = max_offset;


        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);

        // Render chat log
        int render_x = MARGIN_X;
        int current_y = MARGIN_Y - view_y_offset;  // Start from top, apply scroll.
        for (int entry_idx = 0; entry_idx < chat_log_size; entry_idx++) {
            ChatEntry* entry = chat_log[entry_idx];
            if (!entry) continue;
            if (current_y > screen_height + 100) break;  // Skip off-screen early.
            if (current_y + entry->rendered_height < -100) {
                current_y += entry->rendered_height;  // Skip if above view.
                continue;
            }
            current_y = render_chat_entry(r, entry, render_x, current_y);
        }

        SDL_RenderPresent(r);
        SDL_Delay(DELAY);
    }

    // Cleanup chat log
    for (int i = 0; i < chat_log_size; i++) {
        if (!chat_log[i]) continue;
        ChatEntry* entry = chat_log[i];
        for (int j = 0; j < entry->num_wrapped; j++) {
            free(entry->wrapped_lines[j]);
        }
        free(entry->wrapped_lines);
        free(entry->original_line);
        free(entry);
    }
    free(chat_log);

    // Free GMap pixels
    for (int c = 0; c < 128; c++) {
        if (glyphs[c].pixels) {
            free(glyphs[c].pixels);
            glyphs[c].pixels = NULL;
        }
    }
    if (log_file) fclose(log_file);
    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
