#include <pipewire/pipewire.h>

int main() {
  pw_init(nullptr, nullptr);

  fprintf(stdout,
          "Compiled with libpipewire %s\n"
          "Linked with libpipewire %s\n",
          pw_get_headers_version(), pw_get_library_version());

  return 0;
}