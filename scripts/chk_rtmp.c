#include <libavformat/avformat.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    avformat_network_init();
    printf("cfg=%s\n", avformat_configuration());

    const AVOutputFormat* flv = av_guess_format("flv", NULL, NULL);
    printf("flv_guess=%p\n", (const void*)flv);
    if (flv) printf("flv_name=%s long=%s mime=%s\n",
                    flv->name, flv->long_name ? flv->long_name : "",
                    flv->mime_type ? flv->mime_type : "");

    for (void* it = NULL;;) {
        const AVOutputFormat* o = av_muxer_iterate(&it);
        if (!o) break;
        if (strstr(o->name, "flv")) printf("mux:%s\n", o->name);
    }

    /* list protocols */
    void* opaque = NULL;
    const char* p;
    while ((p = avio_enum_protocols(&opaque, 1)) != NULL) {
        if (strstr(p, "rtmp") || strstr(p, "tcp") || strstr(p, "file")) {
            printf("proto:%s\n", p);
        }
    }
    return 0;
}
