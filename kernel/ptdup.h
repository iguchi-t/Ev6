/* The definition of data structure to manage page table duplications (PTDUP).
 */
/* PTDS: PageTable Data Segment
   Pagetable data segment's layout in 64 bit address length.
   63 ~ 38 -- Hole of virtual address at starting point (26 bits, because MAXVA is 256GB in xv6).
   37 ~ 11 -- Part of PPN corresponding to VA (lower 27 bits, covered 512GB).
   10      -- Order bit (ascending (1) or descending (0)).
   9       -- Represent User bit.
   8 ~  0  -- Number of pages which is managed by PTDS (MAX: 2^9-1 = 511 pages).

   in PTDS, we keep only Userbit.
   Because XWRV are must be 1 in user data, and we truncate dirty/global/access.

   PTED: PageTable Entry Duplication
   PTED's layout in 64 bit address length.
   63 ~ 38 -- Hole virtual address at starting point (26 bits, because MAXVA is 256GB in xv6).
   37 ~ 11 -- Part of PPN corresponding to VA (lower 27 bits, covered 512GB).
    9 ~  0 -- All of flags in PTE.
*/

#define ARRAY_SIZE_64 (PGSIZE / sizeof(uint64))
#define ENTRY_SIZE 512

// Head of pagetable duplications for L2 & L1s.
struct ptdup_head {
  uint64 *l2;       // Address of level-2 page table's duplication.
  uint64 **l1;      // Address of page managing level-1's duplications address like page directory.
  uint64 *l0_ptds;  // Address of top of pagetable data segment list(PTDS list) of level-0 pagetables.
  uint64 *l0_pted;  // Address of top of other individual patetable entries of level-0 pagetables.
  struct spinlock lock;
};

// Extract contents from PTDS.
#define PTDS2VA(entry) ((entry >> 26) & ~0xFFF)
#define VA2PTDS(va) (va << 26)
#define PTDS2PPN(entry) ((entry & 0x3FFFFFF800) >> 2)
#define PPN2PTDS(ppn) ((ppn & ~0x3FF) << 2)
#define UB2PTDS(perm) ((perm & 0x10) << 5)
#define PTDS2UB(entry) ((entry >> 5) & 0x10)
#define PTDS2SIZE(entry) (entry & 0x1FF)
#define ORDER2PTDS(order) (order << 10)
#define PTDS2ORDER(entry) ((entry >> 10) & 0x1)

// Extract from / Add contents to PTED.
#define PTED2VA(entry) ((entry >> 26) & ~0xFFF)
#define VA2PTED(va) (va << 26)
#define PTED2PPN(entry) ((entry & 0x3FFFFFF800) >> 2)
#define PPN2PTED(ppn) ((ppn & ~0x3FF) << 2)
#define PTED2FLAGS(entry) (entry & 0x1FF)

#define PTE_D (1L << 7)  // For initializing as dirty PTE.
