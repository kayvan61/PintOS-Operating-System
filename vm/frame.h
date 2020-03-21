/*
  Frame allocator files
  the init should first grab all the user pages and then
  hand them out using this instead of going through palloc_get_page
 */

#include <stdint.h>
#include <list.h>

typedef struct {
  void* page_ptr;
  void* frame_ptr;
  uint8_t extra_flags;

  struct list_elem allelem;           /* List element for all frame list. */
  
} UserFrameTableEntry;

void frame_alloc_init(void);

void* get_free_frame(void);
