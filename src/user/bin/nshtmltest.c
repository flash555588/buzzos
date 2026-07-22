#include "libc.h"

#include <dom/dom.h>
#include <parser.h>
#include <libcss/libcss.h>

static css_error resolve_url(void *context, const char *base,
                             lwc_string *relative, lwc_string **absolute) {
    (void)context;
    (void)base;
    *absolute = lwc_string_ref(relative);
    return CSS_OK;
}

static int count_elements(dom_document *document, const char *name,
                          uint32_t expected) {
    dom_string *tag = 0;
    dom_nodelist *nodes = 0;
    uint32_t count = 0;
    if (dom_string_create((const uint8_t *)name, strlen(name), &tag) != DOM_NO_ERR)
        return -1;
    dom_exception error =
        dom_document_get_elements_by_tag_name(document, tag, &nodes);
    dom_string_unref(tag);
    if (error != DOM_NO_ERR || !nodes)
        return -1;
    error = dom_nodelist_get_length(nodes, &count);
    dom_nodelist_unref(nodes);
    return error == DOM_NO_ERR && count == expected ? 0 : -1;
}

int main(void) {
    static const uint8_t html[] =
        "<!doctype html><html><head><style>"
        "body{background:#fff} input{color:#123456}"
        "</style></head><body><h1>BuzzOS NetSurf</h1>"
        "<form action='/search'><input name='q'></form>"
        "<img src='/logo.png' alt='logo'></body></html>";
    dom_hubbub_parser_params parser_params = {
        .enc = "UTF-8",
        .fix_enc = true,
        .enable_script = false,
        .msg = 0,
        .script = 0,
        .ctx = 0,
        .daf = 0,
    };
    dom_hubbub_parser *parser = 0;
    dom_document *document = 0;
    if (dom_hubbub_parser_create(&parser_params, &parser, &document) !=
            DOM_HUBBUB_OK ||
        dom_hubbub_parser_parse_chunk(parser, html, sizeof(html) - 1u) !=
            DOM_HUBBUB_OK ||
        dom_hubbub_parser_completed(parser) != DOM_HUBBUB_OK) {
        puts("nshtmltest: HTML parse failed");
        if (parser)
            dom_hubbub_parser_destroy(parser);
        return 1;
    }
    dom_hubbub_parser_destroy(parser);
    int dom_ok = count_elements(document, "form", 1) == 0 &&
                 count_elements(document, "input", 1) == 0 &&
                 count_elements(document, "img", 1) == 0;
    dom_node_unref(document);
    if (!dom_ok) {
        puts("nshtmltest: DOM mismatch");
        return 1;
    }

    static const uint8_t css[] =
        "body { background: #fff; } input { color: #123456; width: 16em; }";
    css_stylesheet_params css_params = {
        .params_version = CSS_STYLESHEET_PARAMS_VERSION_1,
        .level = CSS_LEVEL_DEFAULT,
        .charset = "UTF-8",
        .url = "http://buzzos.test/style.css",
        .title = "test",
        .allow_quirks = false,
        .inline_style = false,
        .resolve = resolve_url,
    };
    css_stylesheet *sheet = 0;
    css_error css_error_code = css_stylesheet_create(&css_params, &sheet);
    if (css_error_code == CSS_OK)
        css_error_code = css_stylesheet_append_data(sheet, css, sizeof(css) - 1u);
    if (css_error_code == CSS_NEEDDATA)
        css_error_code = CSS_OK;
    if (css_error_code == CSS_OK)
        css_error_code = css_stylesheet_data_done(sheet);
    if (sheet)
        css_stylesheet_destroy(sheet);
    if (css_error_code != CSS_OK) {
        printf("nshtmltest: CSS parse failed %d\n", css_error_code);
        return 1;
    }
    puts("nshtmltest: ok HTML DOM CSS form img");
    return 0;
}
