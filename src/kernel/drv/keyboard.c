#include "keyboard.h"
#include "io.h"

#define BUF_SIZE 256
#define EVENT_DOWN 0x8000u

static volatile uint8_t buf[BUF_SIZE];
static volatile int     head, tail;
static volatile int     shift_down;
static volatile int     ctrl_down;
static volatile int     alt_down;
static volatile int     extended_prefix;
static volatile uint16_t event_buf[BUF_SIZE];
static volatile int event_head, event_tail;

static void enqueue_char(char c) {
    int next = (tail + 1) % BUF_SIZE;
    if (next == head)
        return;
    buf[tail] = (uint8_t)c;
    tail = next;
}

static void enqueue_seq(const char *s) {
    while (*s)
        enqueue_char(*s++);
}

static void enqueue_event(uint16_t key, int pressed) {
    int next = (event_tail + 1) % BUF_SIZE;
    if (!key || next == event_head)
        return;
    event_buf[event_tail] = (uint16_t)(key | (pressed ? EVENT_DOWN : 0u));
    event_tail = next;
}

/* US QWERTY scancode → ASCII (scancode set 1, unshifted) */
static const char scancode_ascii[128] = {
    0,   0x1B, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*', 0,   ' ',0,  0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
};

/* US QWERTY scancode → ASCII with Shift held. */
static const char scancode_ascii_shift[128] = {
    0,   0x1B, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*', 0,   ' ',0,  0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
};

void keyboard_init(void) {
    head = tail = 0;
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    extended_prefix = 0;
    event_head = event_tail = 0;
}

void keyboard_handler(uint8_t scancode) {
    if (scancode == 0xE0) {
        extended_prefix = 1;
        return;
    }

    uint8_t code = scancode & 0x7F;
    int released = (scancode & 0x80) != 0;
    int extended = extended_prefix;
    extended_prefix = 0;

    if (code == 0x2A || code == 0x36) {
        shift_down = !released;
        return;
    }
    if (code == 0x1D) {
        ctrl_down = !released;
        return;
    }
    if (code == 0x38) {
        alt_down = !released;
        return;
    }
    if (extended) {
        uint16_t key = 0;
        switch (code) {
        case 0x48: key = 256; break; /* Up */
        case 0x50: key = 257; break; /* Down */
        case 0x4D: key = 258; break; /* Right */
        case 0x4B: key = 259; break; /* Left */
        case 0x47: key = 260; break; /* Home */
        case 0x4F: key = 261; break; /* End */
        case 0x53: key = 262; break; /* Delete */
        default: return;
        }
        enqueue_event(key, !released);
        if (released) return;
        switch (code) {
        case 0x48: enqueue_seq("\x1B[A"); return;
        case 0x50: enqueue_seq("\x1B[B"); return;
        case 0x4D: enqueue_seq("\x1B[C"); return;
        case 0x4B: enqueue_seq("\x1B[D"); return;
        case 0x47: enqueue_seq("\x1B[H"); return;
        case 0x4F: enqueue_seq("\x1B[F"); return;
        case 0x53: enqueue_seq("\x1B[3~"); return;
        default: return;
        }
    }
    uint16_t logical = (uint8_t)scancode_ascii[code];
    enqueue_event(logical, !released);
    if (released) return;
    char c = shift_down ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
    if (c == 0) return;
    if (alt_down) {
        if (scancode_ascii[scancode] == '\t')
            enqueue_char(0x1E); /* desktop Alt+Tab task switch */
        return;
    }
    if (ctrl_down) {
        char base = scancode_ascii[scancode];
        if (base == ' ') {
            enqueue_char(0x1F); /* desktop input-method toggle */
            return;
        }
        if (base >= 'a' && base <= 'z')
            c = (char)(base - 'a' + 1);
        else if (base == '[')
            c = 0x1B;
        else if (base == '\\')
            c = 0x1C;
        else if (base == ']')
            c = 0x1D;
        else
            return;
    }

    enqueue_char(c);
}

int keyboard_getchar(void) {
    if (head == tail) return -1;   /* empty */
    int c = buf[head];
    head = (head + 1) % BUF_SIZE;
    return c;
}

int keyboard_getevent(void) {
    if (event_head == event_tail) return -1;
    int event = event_buf[event_head];
    event_head = (event_head + 1) % BUF_SIZE;
    return event;
}
