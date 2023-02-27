
/*****************************************************************************
 *
 * Copyright (c) Imagination Technologies Ltd.
 *
 * The contents of this file are subject to the MIT license as set out below.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License Version 2 ("GPL")in which case the provisions of
 * GPL are applicable instead of those above.
 *
 * If you wish to allow use of your version of this file only under the terms
 * of GPL, and not to allow others to use your version of this file under the
 * terms of the MIT license, indicate your decision by deleting the provisions
 * above and replace them with the notice and other provisions required by GPL
 * as set out in the file called "GPLHEADER" included in this distribution. If
 * you do not delete the provisions above, a recipient may use your version of
 * this file under the terms of either the MIT license or GPL.
 *
 * This License is also included in this distribution in the file called
 * "MIT_COPYING".
 *
 *****************************************************************************/

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/gfp.h>
#include <linux/device.h>
#include <linux/vmalloc.h>

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include <linux/sprd_ion.h>

#include "sprd_vdsp_mem_core.h"
#include "sprd_vdsp_mem_core_priv.h"

/* this condition is actually true for kernels < 4.4.100 */
#ifndef PHYS_PFN
#define PHYS_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))
#endif

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "sprd-vdsp: [mem_heap]: %d %s: "\
        fmt, current->pid, __func__

static int trace_physical_pages = 0;
static int trace_mmap_fault = 0;

struct buffer_data {
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	enum sprd_vdsp_mem_attr mattr;	/* memory attributes */
	struct vm_area_struct *mapped_vma;
};

static int dmabuf_heap_import(struct device *device, struct heap *heap,
			      size_t size, enum sprd_vdsp_mem_attr attr,
			      uint64_t buf_hnd, struct buffer *buffer)
{
	struct buffer_data *data;
	int ret;
	int buf_fd = (int)buf_hnd;
	struct scatterlist *sgl;

	data = kmalloc(sizeof(struct buffer_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dma_buf = dma_buf_get(buf_fd);
	if (IS_ERR_OR_NULL(data->dma_buf)) {
		pr_err("dma_buf_get fd %d fail\n", buf_fd);
		ret = -EINVAL;
		goto dma_buf_get_failed;
	}

	data->attach = dma_buf_attach(data->dma_buf, device);
	if (IS_ERR(data->attach)) {
		pr_err("dma_buf_attach fd %d\n", buf_fd);
		ret = -EINVAL;
		goto dma_buf_attach_failed;
	}

	data->sgt = dma_buf_map_attachment(data->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(data->sgt)) {
		pr_err("dma_buf_map_attachment fd %d\n", buf_fd);
		ret = -EINVAL;
		goto dma_buf_map_failed;
	}

	sgl = data->sgt->sgl;
	if (trace_physical_pages) {
		while (sgl) {
			pr_debug("phys %#llx length %d\n",
				 (unsigned long long)sg_phys(sgl), sgl->length);
			sgl = sg_next(sgl);
		}
	}
	if (PAGE_SIZE == data->dma_buf->size) {
		sgl = data->sgt->sgl;
		buffer->paddr = sg_phys(sgl);
	}

	data->mattr = attr;
	data->mapped_vma = NULL;
	buffer->priv = data;
	return 0;

dma_buf_map_failed:
	dma_buf_detach(data->dma_buf, data->attach);
dma_buf_attach_failed:
	dma_buf_put(data->dma_buf);
dma_buf_get_failed:
	kfree(data);
	return ret;
}

static void dmabuf_heap_free(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *data = buffer->priv;

	if (buffer->kptr) {
		struct dma_buf *dma_buf = data->dma_buf;
		struct scatterlist *sgl = data->sgt->sgl;

		dma_buf_end_cpu_access(dma_buf, DMA_BIDIRECTIONAL);

		dma_buf_kunmap(dma_buf, sg_nents(sgl), buffer->kptr);
	}

	if (data->mapped_vma)
		data->mapped_vma->vm_private_data = NULL;

	dma_buf_unmap_attachment(data->attach, data->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(data->dma_buf, data->attach);
	dma_buf_put(data->dma_buf);
	kfree(data);
}

static void _mmap_open(struct vm_area_struct *vma)
{
	struct buffer *buffer = vma->vm_private_data;
	struct buffer_data *data = buffer->priv;

	if (!(data->mattr & SPRD_VDSP_MEM_ATTR_UNCACHED)) {
		enum dma_data_direction dma_dir;

		if (vma->vm_flags & VM_WRITE)
			dma_dir = DMA_TO_DEVICE;
		else
			dma_dir = DMA_FROM_DEVICE;

		/* User will read the buffer so invalidate D-cache */
		dma_buf_begin_cpu_access(data->dma_buf,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
					 0 /* start */ ,
					 buffer->actual_size,
#endif
					 dma_dir);
	}
	data->mapped_vma = vma;
}

static void _mmap_close(struct vm_area_struct *vma)
{
	struct buffer *buffer = vma->vm_private_data;
	struct buffer_data *data;

	if (!buffer)
		return;

	data = buffer->priv;

	if (!(data->mattr & SPRD_VDSP_MEM_ATTR_UNCACHED)) {
		/* User may have written to the buffer so flush D-cache */
		dma_buf_end_cpu_access(data->dma_buf,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
				       0 /* start */ ,
				       buffer->actual_size,
#endif
				       DMA_TO_DEVICE);
	}

	data->mapped_vma = NULL;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static vm_fault_t _mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#else
static int _mmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
#endif
	struct buffer *buffer = vma->vm_private_data;
	struct buffer_data *data = buffer->priv;
	struct sg_table *sgt = data->sgt;
	struct scatterlist *sgl;
	pgoff_t curr_offset;
	dma_addr_t phys = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	unsigned long addr = vmf->address;
#else
	unsigned long addr = (unsigned long)vmf->virtual_address;
#endif

	if (trace_mmap_fault) {
		pr_debug("buffer %d (0x%p) vma:%p\n", buffer->id, buffer, vma);
		pr_debug("vm_start %#lx vm_end %#lx total size %ld\n",
			 vma->vm_start, vma->vm_end,
			 vma->vm_end - vma->vm_start);
	}

	curr_offset = addr - vma->vm_start;

	sgl = sgt->sgl;
	while (sgl) {
		phys = sg_phys(sgl);
		if (curr_offset < sgl->length)
			break;
		curr_offset -= sgl->length;
		sgl = sg_next(sgl);
	}
	phys += curr_offset;	/* set to middle of current block */
	if (trace_mmap_fault)
		pr_debug("vmf pgoff:%#lx vmf addr:%lx phys:%#llx\n", vmf->pgoff,
			 addr, (unsigned long long)phys);

	{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
		unsigned long pfn = PHYS_PFN(phys);
#else
		pfn_t pfn = {
			.val = PHYS_PFN(phys)
		};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
		return vmf_insert_mixed(vma, addr, pfn);
#else
		{
			int err = vm_insert_mixed(vma, addr, pfn);

			switch (err) {
			case 0:
			case -EAGAIN:
			case -ERESTARTSYS:
			case -EINTR:
			case -EBUSY:
				return VM_FAULT_NOPAGE;
			case -ENOMEM:
				return VM_FAULT_OOM;
			}

			return VM_FAULT_SIGBUS;
		}
#endif
	}
}

/* vma ops->fault handler is used to track user space mappings
 * (inspired by other gpu/drm drivers from the kernel source tree)
 * to properly call dma_sync_* ops when the mapping is destroyed
 * (when user calls unmap syscall).
 * vma flags are used to choose a correct dma mapping.
 * By default use DMA_BIDIRECTONAL mapping type (kernel space only).
 * The above facts allows us to do automatic cache flushing/invalidation.
 *
 * Examples:
 *  mmap() -> .open -> invalidate buffer cache
 *  .. read content from buffer
 *  unmap() -> .close -> do nothing
 *
 *  mmap() -> .open -> do nothing
 *  .. write content to buffer
 *  unmap() -> .close -> flush buffer cache
 */
static struct vm_operations_struct dmabuf_heap_mmap_vm_ops = {
	.open = _mmap_open,
	.close = _mmap_close,
	.fault = _mmap_fault,
};

static int dmabuf_heap_map_um(struct heap *heap, struct buffer *buffer,
			      struct vm_area_struct *vma)
{
	struct buffer_data *data = buffer->priv;

	/* CACHED by default */
	if (data->mattr & SPRD_VDSP_MEM_ATTR_WRITECOMBINE)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else if (data->mattr & SPRD_VDSP_MEM_ATTR_UNCACHED)
		WARN_ONCE(1, "Uncached not allowed");
	/*vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot); */

	vma->vm_ops = &dmabuf_heap_mmap_vm_ops;
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;
	vma->vm_private_data = buffer;
	vma->vm_pgoff = 0;

	_mmap_open(vma);

	return 0;
}

static int dmabuf_heap_map_km(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *data = buffer->priv;
	struct dma_buf *dma_buf = data->dma_buf;
	int ret;

	if (buffer->kptr) {
		pr_warn("called for already mapped buffer %d\n", buffer->id);
		return 0;
	}

	ret = dma_buf_begin_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	if (ret) {
		pr_err("begin_cpu_access fd %d\n", buffer->id);
		return ret;
	}

	buffer->kptr = dma_buf_kmap(dma_buf, 0 /*sg_nents(sgl) */ );

	if (!buffer->kptr) {
		pr_err("dma_buf_kmap failed!\n");
		return -EFAULT;
	}

	return 0;
}

static int dmabuf_heap_unmap_km(struct heap *heap, struct buffer *buffer)
{
	struct buffer_data *data = buffer->priv;
	struct dma_buf *dma_buf = data->dma_buf;
	struct scatterlist *sgl = data->sgt->sgl;

	if (!buffer->kptr) {
		pr_warn("called for unmapped buffer %d\n", buffer->id);
		return 0;
	}

	dma_buf_end_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	dma_buf_kunmap(dma_buf, sg_nents(sgl), buffer->kptr);

	buffer->kptr = NULL;
	return 0;
}

static int dmabuf_get_sg_table(struct heap *heap, struct buffer *buffer,
			       struct sg_table **sg_table)
{
	struct buffer_data *data = buffer->priv;

	*sg_table = data->sgt;
	return 0;
}

static void dmabuf_heap_destroy(struct heap *heap)
{
	return;
}

static struct heap_ops dmabuf_heap_ops = {
	.alloc = NULL,
	.import = dmabuf_heap_import,
	.free = dmabuf_heap_free,
	.map_um = dmabuf_heap_map_um,
	.unmap_um = NULL,
	.map_km = dmabuf_heap_map_km,
	.unmap_km = dmabuf_heap_unmap_km,
	.get_sg_table = dmabuf_get_sg_table,
	.get_page_array = NULL,
	.sync_cpu_to_dev = NULL,
	.sync_dev_to_cpu = NULL,
	.destroy = dmabuf_heap_destroy,
};

int sprd_vdsp_mem_dmabuf_init(const struct heap_config *heap_cfg,
			      struct heap *heap)
{
	heap->ops = &dmabuf_heap_ops;
	return 0;
}
