#include <stddef.h>
#include <string.h>
extern int gitstore_rust_image_probe(const unsigned char *,size_t,const unsigned char *,size_t);
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    return gitstore_rust_image_probe((const unsigned char*)argv[1],strlen(argv[1]),(const unsigned char*)argv[2],strlen(argv[2]));
}
