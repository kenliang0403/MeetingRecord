#include <dlfcn.h>
#include <stdio.h>

int main() {
    void* handle = dlopen("/usr/local/lib/pwlib/codecs/audio/g722_audio_pwplugin.so", RTLD_NOW);
    if (!handle) {
        printf("Error: %s\n", dlerror());
        return 1;
    }
    printf("Successfully loaded plugin\n");
    dlclose(handle);
    return 0;
}
