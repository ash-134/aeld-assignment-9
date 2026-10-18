#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
#include "aesd-circular-buffer.h"

struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer, size_t char_offset, size_t *entry_offset_byte_rtn )
{
    uint8_t count = 0;
    uint8_t index;
    if (!buffer || !entry_offset_byte_rtn) return NULL;
    if (!buffer->full && (buffer->in_offs == buffer->out_offs)) return NULL;
    index = buffer->out_offs;
    while (count < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        if (buffer->entry[index].buffptr != NULL) {
            if (char_offset < buffer->entry[index].size) {
                *entry_offset_byte_rtn = char_offset;
                return &buffer->entry[index];
            }
            char_offset -= buffer->entry[index].size;
        }
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        count++;
        if (!buffer->full && index == buffer->in_offs) break;
    }
    return NULL;
}

const char *aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const  struct aesd_buffer_entry *add_entry)
{
    const char *ret_ptr = NULL;
    if (!buffer || !add_entry) return NULL;
    if (buffer->full) {
        ret_ptr = buffer->entry[buffer->in_offs].buffptr;
        buffer->out_offs = (buffer->out_offs + 1) 	 AESDCHAR_MAX_WRITE_OPERATIONS_SUPPERTED;
    }
    buffer->entry[buffer->in_offs] = *add_entry;
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    if (buffer->in_offs == buffer->out_offs) buffer->full = true;
    return ret_ptr;
}

void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer, 0, sizeof(struct aesd_circular_buffer));
}
