#include "/home/swishy/Development/Source/velho/usr/lib/zen/mem.c"
int main(void) {
 struct membuf b = {0};
 for (size_t n=1; n<=MEM_SLAB_SIZE/MEM_BLK_UNIT; ++n) {
  b.max=n; b.lastmask=UINTPTR_MAX >> ((-n) & (MEM_BUF_MAP_BITS-1));
  for(size_t w=0; w*MEM_BUF_MAP_BITS<n; ++w) {
   size_t c=min(n-w*MEM_BUF_MAP_BITS,MEM_BUF_MAP_BITS);
   uintptr_t old=UINTPTR_MAX>>(MEM_BUF_MAP_BITS-c);
   if(!w) old &= ~MEM_BUF_FLAGS;
   if(memwordmask(&b,w)!=old) return 1;
  }
 }
 return 0;
}
