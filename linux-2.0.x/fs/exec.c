/*
 *  linux/fs/exec.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */
/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 *
 * Demand loading changed July 1993 by Eric Youngdale.   Use mmap instead,
 * current->executable is only used by the procfs.  This allows a dispatch
 * table to check for several different types  of binary formats.  We keep
 * trying until we recognize the file or we run out of supported binary
 * formats. 
 */

/*
 * uClinux revisions for NO_MM
 * Copyright (C) 1998  Kenneth Albanowski <kjahds@kjahds.com>,
 */  

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/a.out.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/user.h>
#include <linux/malloc.h>
#include <linux/binfmts.h>
#include <linux/personality.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

#include <linux/config.h>
#ifdef CONFIG_KERNELD
#include <linux/kerneld.h>
#endif

asmlinkage int sys_exit(int exit_code);
asmlinkage int sys_brk(unsigned long);

/*
 * Here are the actual binaries that will be accepted:
 * add more with "register_binfmt()" if using modules...
 *
 * These are defined again for the 'real' modules if you are using a
 * module definition for these routines.
 */

static struct linux_binfmt *formats = (struct linux_binfmt *) NULL;

void binfmt_setup(void)
{
#ifdef CONFIG_BINFMT_FLAT
	init_flat_binfmt();
#endif

#ifdef CONFIG_BINFMT_ELF
	init_elf_binfmt();
#endif

#ifdef CONFIG_BINFMT_AOUT
	init_aout_binfmt();
#endif

#ifdef CONFIG_BINFMT_JAVA
	init_java_binfmt();
#endif
	/* This cannot be configured out of the kernel */
	init_script_binfmt();
}

int register_binfmt(struct linux_binfmt * fmt)
{
	struct linux_binfmt ** tmp = &formats;

	if (!fmt)
		return -EINVAL;
	if (fmt->next)
		return -EBUSY;
	while (*tmp) {
		if (fmt == *tmp)
			return -EBUSY;
		tmp = &(*tmp)->next;
	}
	fmt->next = formats;
	formats = fmt;
	return 0;	
}

#ifdef CONFIG_MODULES
int unregister_binfmt(struct linux_binfmt * fmt)
{
	struct linux_binfmt ** tmp = &formats;

	while (*tmp) {
		if (fmt == *tmp) {
			*tmp = fmt->next;
			return 0;
		}
		tmp = &(*tmp)->next;
	}
	return -EINVAL;
}
#endif	/* CONFIG_MODULES */

int open_inode(struct inode * inode, int mode)
{
	int fd;

	if (!inode->i_op || !inode->i_op->default_file_ops)
		return -EINVAL;
	fd = get_unused_fd();
	if (fd >= 0) {
		struct file * f = get_empty_filp();
		if (!f) {
			put_unused_fd(fd);
			return -ENFILE;
		}
		f->f_flags = mode;
		f->f_mode = (mode+1) & O_ACCMODE;
		f->f_inode = inode;
		f->f_pos = 0;
		f->f_reada = 0;
		f->f_op = inode->i_op->default_file_ops;
		if (f->f_op->open) {
			int error = f->f_op->open(inode,f);
			if (error) {
				f->f_count--;
				put_unused_fd(fd);
				return error;
			}
		}
		current->files->fd[fd] = f;
		inode->i_count++;
	}
	return fd;
}

/*
 * Note that a shared library must be both readable and executable due to
 * security reasons.
 *
 * Also note that we take the address to load from from the file itself.
 */
asmlinkage int sys_uselib(const char * library)
{
	int fd, retval;
	struct file * file;
	struct linux_binfmt * fmt;

	fd = sys_open(library, 0, 0);
	if (fd < 0)
		return fd;
	file = current->files->fd[fd];
	retval = -ENOEXEC;
	if (file && file->f_inode && file->f_op && file->f_op->read) {
		for (fmt = formats ; fmt ; fmt = fmt->next) {
			int (*fn)(int) = fmt->load_shlib;
			if (!fn)
				continue;
			retval = fn(fd);
			if (retval != -ENOEXEC)
				break;
		}
	}
	sys_close(fd);
  	return retval;
}

#ifndef NO_MM

/*
 * count() counts the number of arguments/envelopes
 */
static int count(void *base, int size, int max)
{
	int error, i = 0;
	void *tmp = base;
	unsigned long length = 0, chunk = size, limit;
	int grow = 1;

	if (!tmp) return 0;

	limit = PAGE_SIZE - ((unsigned long)tmp & (PAGE_SIZE - 1));
	error = verify_area(VERIFY_READ, tmp, limit);
	if (error) limit = 0;

	do {
		if (length >= limit)
		do {
			if (!grow) {
				if (chunk <= sizeof(char *))
					return -EFAULT;
				chunk >>= 1;
			}
			error = verify_area(VERIFY_READ, tmp, chunk);
			if (error) grow = 0; else {
				limit += chunk;
				if (grow) chunk <<= 1;
			}
		} while (error);

		if (size == 1) {
			do {
				if (!get_user(((char *)tmp)++)) goto out;
				if (++i > max) return -E2BIG;
			} while (i < limit);
			length = i;
		} else {
			do {
				if (!get_user(((char **)tmp)++)) goto out;
				if ((length += size) > max) return -E2BIG;
				i++;
			} while (length < limit);
		}
	} while (1);

out:
	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 * 
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 * 
 * We do this by playing games with the fs segment register.  Since it
 * is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem)
{
	char *tmp, *pag = NULL;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if ((long)p <= 0)
		return p;	/* bullet-proofing */
	new_fs = get_ds();
	old_fs = get_fs();
	if (from_kmem==2)
		set_fs(new_fs);
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		if (!(tmp = get_user(argv+argc)))
			panic("VFS: argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len = count(tmp, 1, p);
		if (len < 0 || len >= p) {	/* EFAULT or E2BIG */
			set_fs(old_fs);
			return len < 0 ? len : -E2BIG;
		}
		tmp += ++len;
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) {
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page(GFP_USER))) 
					return -EFAULT;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			if (len == 0 || offset == 0)
			  *(pag + offset) = get_user(tmp);
			else {
			  int bytes_to_copy = (len > offset) ? offset : len;
			  tmp -= bytes_to_copy;
			  p -= bytes_to_copy;
			  offset -= bytes_to_copy;
			  len -= bytes_to_copy;
			  memcpy_fromfs(pag + offset, tmp, bytes_to_copy + 1);
			}
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}

unsigned long setup_arg_pages(unsigned long p, struct linux_binprm * bprm)
{
	unsigned long stack_base;
	struct vm_area_struct *mpnt;
	int i;

	stack_base = STACK_TOP - MAX_ARG_PAGES*PAGE_SIZE;

	p += stack_base;
	if (bprm->loader)
		bprm->loader += stack_base;
	bprm->exec += stack_base;

	mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt), GFP_KERNEL);
	if (mpnt) {
		mpnt->vm_mm = current->mm;
		mpnt->vm_start = PAGE_MASK & (unsigned long) p;
		mpnt->vm_end = STACK_TOP;
		mpnt->vm_page_prot = PAGE_COPY;
		mpnt->vm_flags = VM_STACK_FLAGS;
		mpnt->vm_ops = NULL;
		mpnt->vm_offset = 0;
		mpnt->vm_inode = NULL;
		mpnt->vm_pte = 0;
		insert_vm_struct(current->mm, mpnt);
		current->mm->total_vm = (mpnt->vm_end - mpnt->vm_start) >> PAGE_SHIFT;

		for (i = 0 ; i < MAX_ARG_PAGES ; i++) {
			if (bprm->page[i]) {
				current->mm->rss++;
				put_dirty_page(current,bprm->page[i],stack_base);
			}
			stack_base += PAGE_SIZE;
		}
	} else {
		/*
		 * This one is tricky. We are already in the new context, so we cannot
		 * return with -ENOMEM. So we _have_ to deallocate argument pages here,
		 * if there is no VMA, they wont be freed at exit_mmap() -> memory leak.
		 *
		 * User space then gets a SIGSEGV when it tries to access argument pages.
		 */
		for (i = 0 ; i < MAX_ARG_PAGES ; i++) {
			if (bprm->page[i]) {
				free_page(bprm->page[i]);
				bprm->page[i] = 0;
			}
		}
	}

	return p;
}

#else /* !NO_MM */

/*
 * count() counts the number of arguments/envelopes
 * Ignore size and max for now, not setup for NO_MM.
 */

static int count(void *base, int size, int max)
{
	int error, i = 0;
	char ** tmp, *p;

	if ((tmp = base) != NULL) {
		error = verify_area(VERIFY_READ, tmp, sizeof(char *));
		if (error)
			return error;
		while ((p = get_user(tmp++)) != NULL) {
			i++;
			error = verify_area(VERIFY_READ, p, 1);
			if (error)
				return error;
		}
	}
	return i;
}

/*
 *	We need to allocate/copy some argv style arrays,  so we build it up
 *	here with one alloc.  The caller has to free the result themselves.
 */

char **
copy_strings(int argc, char **argv)
{
	int		i, n;
	char	**_argv, *sp;

	n = (argc + 1) * sizeof(char *);
	for (i = 0; i < argc; i++)
		n += strlen(argv[i]) + 1;
	
	_argv = kmalloc(n, GFP_KERNEL);
	if (!_argv)
		return(_argv);
	
	sp = ((char *) _argv) + ((argc + 1) * sizeof(char *));

	for (i = 0; i < argc; i++) {
		_argv[i] = sp;
		n = strlen(argv[i]) + 1;
		memcpy(sp, argv[i], n);
		sp += n;
	}
		
	_argv[i] = NULL;
	return(_argv);
}

#endif /* !NO_MM */

/*
 * Read in the complete executable. This is used for "-N" files
 * that aren't on a block boundary, and for files on filesystems
 * without bmap support.
 */
int read_exec(struct inode *inode, unsigned long offset,
	char * addr, unsigned long count, int to_kmem)
{
	struct file file;
	int result = -ENOEXEC;

	if (!inode->i_op || !inode->i_op->default_file_ops)
		goto end_readexec;
	file.f_mode = 1;
	file.f_flags = 0;
	file.f_count = 1;
	file.f_inode = inode;
	file.f_pos = 0;
	file.f_reada = 0;
	file.f_op = inode->i_op->default_file_ops;
	if (file.f_op->open)
		if (file.f_op->open(inode,&file))
			goto end_readexec;
	if (!file.f_op || !file.f_op->read)
		goto close_readexec;
	if (file.f_op->lseek) {
		if (file.f_op->lseek(inode,&file,offset,0) != offset)
 			goto close_readexec;
	} else
		file.f_pos = offset;
	if (to_kmem) {
		unsigned long old_fs = get_fs();
		set_fs(get_ds());
		result = file.f_op->read(inode, &file, addr, count);
		set_fs(old_fs);
	} else {
		result = verify_area(VERIFY_WRITE, addr, count);
		if (result)
			goto close_readexec;
		result = file.f_op->read(inode, &file, addr, count);
	}
close_readexec:
	if (file.f_op->release)
		file.f_op->release(inode,&file);
end_readexec:
	return result;
}

#ifndef NO_MM
static int exec_mmap(void)
{
	/*
	 * The clear_page_tables done later on exec does the right thing
	 * to the page directory when shared, except for graceful abort
	 */
	if (current->mm->count > 1) {
		struct mm_struct *old_mm, *mm = kmalloc(sizeof(*mm), GFP_KERNEL);
		if (!mm)
			return -ENOMEM;

		*mm = *current->mm;
		mm->def_flags = 0;	/* should future lockings be kept? */
		mm->count = 1;
		mm->mmap = NULL;
		mm->mmap_avl = NULL;
		mm->total_vm = 0;
		mm->rss = 0;

		old_mm = current->mm;
		current->mm = mm;
		if (new_page_tables(current)) {
			/* The pgd belongs to the parent ... don't free it! */
			mm->pgd = NULL;
			current->mm = old_mm;
			exit_mmap(mm);
			kfree(mm);
			return -ENOMEM;
		}

		if ((old_mm != &init_mm) && (!--old_mm->count)) {
			/*
			 * all threads exited while we were sleeping, 'old_mm' is held
			 * by us exclusively, lets get rid of it:
			 */
			exit_mmap(old_mm);
			free_page_tables(old_mm);
			kfree(old_mm);
		}

		return 0;
	}
	flush_cache_mm(current->mm);
	exit_mmap(current->mm);
	clear_page_tables(current);
	flush_tlb_mm(current->mm);

	return 0;
}
#else /* NO_MM */
static int exec_mmap(void)
{
	if (current->mm->count > 1) {
		struct mm_struct *old_mm, *mm = kmalloc(sizeof(*mm), GFP_KERNEL);
		if (!mm)
			return -ENOMEM;

		*mm = *current->mm;
		mm->def_flags = 0;	/* should future lockings be kept? */
		mm->count = 1;
		mm->total_vm = 0;
		mm->rss = 0;
		mm->executable = 0;
		mm->tblock.rblock = 0;
		mm->tblock.next = 0;
		
		old_mm = current->mm;
		current->mm = mm;

		if ((old_mm != &init_mm) && (!--old_mm->count)) {
			/*
			 * all threads exited while we were sleeping, 'old_mm' is held
			 * by us exclusively, lets get rid of it:
			 */
			exit_mmap(old_mm);
			kfree(old_mm);
		}
		
		return 0;
	}
	exit_mmap(current->mm);

	return 0;
}

#endif /* NO_MM */

/*
 * These functions flushes out all traces of the currently running executable
 * so that a new one can be started
 */

static inline void flush_old_signals(struct signal_struct *sig)
{
	int i;
	struct sigaction * sa = sig->action;

	for (i=32 ; i != 0 ; i--) {
		sa->sa_mask = 0;
		sa->sa_flags = 0;
		if (sa->sa_handler != SIG_IGN)
			sa->sa_handler = NULL;
		sa++;
	}
}

static inline void flush_old_files(struct files_struct * files)
{
	unsigned long j;

	j = 0;
	for (;;) {
		unsigned long set, i;

		i = j * __NFDBITS;
		if (i >= NR_OPEN)
			break;
		set = files->close_on_exec.fds_bits[j];
		files->close_on_exec.fds_bits[j] = 0;
		j++;
		for ( ; set ; i++,set >>= 1) {
			if (set & 1)
				sys_close(i);
		}
	}
}

int flush_old_exec(struct linux_binprm * bprm)
{
	int i;
	int ch;
	char * name;

	if (current->euid == current->uid && current->egid == current->gid)
		current->dumpable = 1;
	name = bprm->filename;
	for (i=0; (ch = *(name++)) != '\0';) {
		if (ch == '/')
			i = 0;
		else
			if (i < 15)
				current->comm[i++] = ch;
	}
	current->comm[i] = '\0';

	/* Release all of the old mmap stuff. */
	if (exec_mmap())
		return -ENOMEM;

	flush_thread();

	if (bprm->e_uid != current->euid || bprm->e_gid != current->egid || 
	    permission(bprm->inode,MAY_READ))
		current->dumpable = 0;

	flush_old_signals(current->sig);
	flush_old_files(current->files);

	return 0;
}

/* 
 * Fill the binprm structure from the inode. 
 * Check permissions, then read the first 512 bytes
 */
int prepare_binprm(struct linux_binprm *bprm)
{
	int mode;
	int retval,id_change;

	mode = bprm->inode->i_mode;
	if (!S_ISREG(mode))			/* must be regular file */
		return -EACCES;
	if (!(mode & 0111))			/* with at least _one_ execute bit set */
		return -EACCES;
	if (IS_NOEXEC(bprm->inode))		/* FS mustn't be mounted noexec */
		return -EACCES;
	if (!bprm->inode->i_sb)
		return -EACCES;
	if ((retval = permission(bprm->inode, MAY_EXEC)) != 0)
		return retval;
	/* better not execute files which are being written to */
	if (bprm->inode->i_writecount > 0)
		return -ETXTBSY;

	bprm->e_uid = current->euid;
	bprm->e_gid = current->egid;
	id_change = 0;

	/* Set-uid? */
	if (mode & S_ISUID) {
		bprm->e_uid = bprm->inode->i_uid;
		if (bprm->e_uid != current->euid)
			id_change = 1;
	}

	/* Set-gid? */
	/*
	 * If setgid is set but no group execute bit then this
	 * is a candidate for mandatory locking, not a setgid
	 * executable.
	 */
	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
		bprm->e_gid = bprm->inode->i_gid;
		if (!in_group_p(bprm->e_gid))
			id_change = 1;
	}

	if (id_change) {
		/* We can't suid-execute if we're sharing parts of the executable */
		/* or if we're being traced (or if suid execs are not allowed)    */
		/* (current->mm->count > 1 is ok, as we'll get a new mm anyway)   */
		if (IS_NOSUID(bprm->inode)
		    || (current->flags & PF_PTRACED)
		    || (current->fs->count > 1)
		    || (current->sig->count > 1)
		    || (current->files->count > 1)) {
			if (!suser())
				return -EPERM;
		}

		/*
		 * Increment the privileged execution counter, so that our
		 * old children know not to send bad exit_signal's to us.
		 */
		if (!++current->priv) {
			struct task_struct *p;

			/*
			 * The counter can't really overflow with real-world
			 * programs (and it has to be the privileged program
			 * itself that causes the overflow), but we handle
			 * this case anyway, just for correctness.
			 */
			for_each_task(p) {
				if (p->p_pptr == current) {
					p->ppriv = 0;
					current->priv = 1;
				}
			}
		}
	}

	memset(bprm->buf,0,sizeof(bprm->buf));
	return read_exec(bprm->inode,0,bprm->buf,128,1);
}

#ifndef NO_MM
void remove_arg_zero(struct linux_binprm *bprm)
{
	if (bprm->argc) {
		unsigned long offset;
		char * page;
		offset = bprm->p % PAGE_SIZE;
		page = (char*)bprm->page[bprm->p/PAGE_SIZE];
		while(bprm->p++,*(page+offset++))
			if(offset==PAGE_SIZE){
				offset=0;
				page = (char*)bprm->page[bprm->p/PAGE_SIZE];
			}
		bprm->argc--;
	}
}
#endif /* !NO_MM */

/*
 * cycle the list of binary formats handler, until one recognizes the image
 */
int search_binary_handler(struct linux_binprm *bprm,struct pt_regs *regs)
{
	int try,retval=0;
	struct linux_binfmt *fmt;
#ifdef __alpha__
	/* handle /sbin/loader.. */
	{
	    struct exec * eh = (struct exec *) bprm->buf;

	    if (!bprm->loader && eh->fh.f_magic == 0x183 &&
		(eh->fh.f_flags & 0x3000) == 0x3000)
	    {
		char * dynloader[] = { "/sbin/loader" };
		iput(bprm->inode);
		bprm->dont_iput = 1;
		remove_arg_zero(bprm);
		bprm->p = copy_strings(1, dynloader, bprm->page, bprm->p, 2);
		if ((long)bprm->p < 0)
			return (long)bprm->p;
		bprm->argc++;
		bprm->loader = bprm->p;
		retval = open_namei(dynloader[0], 0, 0, &bprm->inode, NULL);
		if (retval)
			return retval;
		bprm->dont_iput = 0;
		retval = prepare_binprm(bprm);
		if (retval<0)
			return retval;
		/* should call search_binary_handler recursively here,
		   but it does not matter */
	    }
	}
#endif
	for (try=0; try<2; try++) {
		for (fmt = formats ; fmt ; fmt = fmt->next) {
			int (*fn)(struct linux_binprm *, struct pt_regs *) = fmt->load_binary;
			if (!fn)
				continue;
			retval = fn(bprm, regs);
			if (retval >= 0) {
				if(!bprm->dont_iput)
					iput(bprm->inode);
				bprm->dont_iput=1;
				current->did_exec = 1;
				return retval;
			}
			if (retval != -ENOEXEC)
				break;
			if (bprm->dont_iput) /* We don't have the inode anymore*/
				return retval;
		}
		if (retval != -ENOEXEC) {
			break;
#ifdef CONFIG_KERNELD
		}else{
#define printable(c) (((c)=='\t') || ((c)=='\n') || (0x20<=(c) && (c)<=0x7e))
			char modname[20];
			if (printable(bprm->buf[0]) &&
			    printable(bprm->buf[1]) &&
			    printable(bprm->buf[2]) &&
			    printable(bprm->buf[3]))
				break; /* -ENOEXEC */
			sprintf(modname, "binfmt-%hd", *(short*)(&bprm->buf));
			request_module(modname);
#endif
		}
	}
	return retval;
}


/*
 * sys_execve() executes a new program.
 */
int do_execve(char * filename, char ** argv, char ** envp, struct pt_regs * regs)
{
	struct linux_binprm bprm;
	int retval;
#ifndef NO_MM
	int i;
#endif /* !NO_MM */

#ifndef NO_MM
	bprm.p = PAGE_SIZE*MAX_ARG_PAGES-sizeof(void *);
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		bprm.page[i] = 0;
#endif /* !NO_MM */
	retval = open_namei(filename, 0, 0, &bprm.inode, NULL);
	if (retval)
		return retval;
	bprm.filename = filename;
	bprm.sh_bang = 0;
	bprm.loader = 0;
	bprm.exec = 0;
	bprm.dont_iput = 0;
	if ((bprm.argc = count(argv, sizeof(char *), bprm.p)) < 0)
		return bprm.argc;
	if ((bprm.envc = count(envp, sizeof(char *), bprm.p)) < 0)
		return bprm.envc;

	retval = prepare_binprm(&bprm);
	
#ifndef NO_MM
	if(retval>=0) {
		bprm.p = copy_strings(1, &bprm.filename, bprm.page, bprm.p, 2);
		bprm.exec = bprm.p;
		bprm.p = copy_strings(bprm.envc,envp,bprm.page,bprm.p,0);
		bprm.p = copy_strings(bprm.argc,argv,bprm.page,bprm.p,0);
		if ((long)bprm.p < 0)
			retval = (long)bprm.p;
	}
#else /* NO_MM */
	/*
	 * we have to make copies of these strings,  otherwise we re-use
	 * them after they are freed (when we clean up the old exec)
	 */
	bprm.envp = copy_strings(bprm.envc, envp);
	bprm.argv = copy_strings(bprm.argc, argv);
#endif /* NO_MM */

	if(retval>=0)
		retval = search_binary_handler(&bprm,regs);
	if(retval>=0) {
#ifdef NO_MM
		/* Wake up parent that vforked me */
		if (current->vforkwoken == 0) {
			current->vforkwoken = 1;
			wake_up(&current->p_opptr->mm->vforkwait);
		}
		kfree(bprm.envp);
		kfree(bprm.argv);
#endif /* NO_MM */
		/* execve success */
		return retval;
	}

	/* Something went wrong, return the inode and free the argument pages*/
	if(!bprm.dont_iput)
		iput(bprm.inode);
#ifndef NO_MM
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(bprm.page[i]);
#else
	kfree(bprm.envp);
	kfree(bprm.argv);
#endif /* !NO_MM */
	return(retval);
}
