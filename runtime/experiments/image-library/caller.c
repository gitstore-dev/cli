/* Build explicitly with a generated archive header, not through the Go package. */
#include "image-probe.h"
int main(void) { return ImageLibraryProbe(); }
