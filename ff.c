/*--------------------------------------------------------------------------/
/  FatFs - FAT file system module  R0.05                     (C)ChaN, 2007
/---------------------------------------------------------------------------/
/ The FatFs module is an experimenal project to implement FAT file system to
/ cheap microcontrollers. This is a free software and is opened for education,
/ research and development under license policy of following trems.
/
/  Copyright (C) 2007, ChaN, all right reserved.
/
/ * The FatFs module is a free software and there is no warranty.
/ * You can use, modify and/or redistribute it for personal, non-profit or
/   profit use without any restriction under your responsibility.
/ * Redistributions of source code must retain the above copyright notice.
/
/---------------------------------------------------------------------------/
/  Feb 26, 2006  R0.00  Prototype.
/  Apr 29, 2006  R0.01  First stable version.
/  Jun 01, 2006  R0.02  Added FAT12 support.
/                       Removed unbuffered mode.
/                       Fixed a problem on small (<32M) patition.
/  Jun 10, 2006  R0.02a Added a configuration option (_FS_MINIMUM).
/  Sep 22, 2006  R0.03  Added f_rename().
/                       Changed option _FS_MINIMUM to _FS_MINIMIZE.
/  Dec 11, 2006  R0.03a Improved cluster scan algolithm to write files fast.
/                       Fixed f_mkdir() creates incorrect directory on FAT32.
/  Feb 04, 2007  R0.04  Supported multiple drive system.
/                       Changed some interfaces for multiple drive system.
/                       Changed f_mountdrv() to f_mount().
/                       Added f_mkfs().
/  Apr 01, 2007  R0.04a Supported multiple partitions on a plysical drive.
/                       Added a capability of extending file size to f_lseek().
/                       Added minimization level 3.
/                       Fixed an endian sensitive code in f_mkfs().
/  May 05, 2007  R0.04b Added a configuration option _USE_NTFLAG.
/                       Added FSInfo support.
/                       Fixed DBCS name can result FR_INVALID_NAME.
/                       Fixed short seek (<= csize) collapses the file object.
/  Aug 25, 2007  R0.05  Changed arguments of f_read(), f_write() and f_mkfs().
/                       Fixed f_mkfs() on FAT32 creates incorrect FSInfo.
/                       Fixed f_mkdir() on FAT32 creates incorrect directory.
/---------------------------------------------------------------------------*/

#include <avr/pgmspace.h>
#include <string.h>
#include "ff.h"         /* FatFs declarations */
#include "sdcard.h"     /* Include file for user provided disk functions */


/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/
static
FATFS *FatFs[_DRIVES];  /* Pointer to the file system objects (logical drives) */
static
WORD fsid;              /* File system mount ID */

#if _USE_LFN!=0
static const PROGMEM
BYTE LFN_pos[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
#endif

#if _USE_1_BUF != 0
# define FSBUF static_buf
static 
BUF static_buf;
#else
# define FSBUF (fs->buf)
#endif

#if _USE_FS_BUF == 1
# define FPBUF FSBUF
#else
# define FPBUF (fp->buf)
#endif

/*-----------------------------------------------------------------------*/
/* Change window offset                                                  */
/*-----------------------------------------------------------------------*/

static
BOOL move_window (      /* TRUE: successful, FALSE: failed */
  FATFS *fs,            /* File system object */
  BUF *buf,
  DWORD sector          /* Sector number to make apperance in the fs->buf.data[] */
)                       /* Move to zero only writes back dirty window */
{
  DWORD wsect;


  //printf("curr sector=%d, new sector=%d, dirty=%d\n",buf->sect,sector,buf->dirty);
  wsect = buf->sect;
#if _USE_1_BUF != 0
  if (wsect != sector || fs->idx != buf->fsidx) {   /* Changed current window */
#else
  if (wsect != sector) {                            /* Changed current window */
#endif
#if !_FS_READONLY
    BYTE n;
    if (buf->dirty) {                               /* Write back dirty window if needed */
      if (disk_write(fs->drive, buf->data, wsect, 1) != RES_OK)
        return FALSE;
      buf->dirty = FALSE;
      if (wsect < (fs->fatbase + fs->sects_fat)) {  /* In FAT area */
        for (n = fs->n_fats; n >= 2; n--) {         /* Reflect the change to FAT copy */
          wsect += fs->sects_fat;
          disk_write(fs->drive, buf->data, wsect, 1);
        }
      }
    }
#endif
    if (sector) {
      if (disk_read(fs->drive, buf->data, sector, 1) != RES_OK)
        return FALSE;
      buf->sect = sector;
#if _USE_1_BUF != 0
      buf->fsidx=fs->idx;
#endif
    }
  }
  return TRUE;
}




static
BOOL move_fs_window(
  FATFS* fs,
  DWORD  sector
)
{
#if _USE_1_BUF != 0
  BOOL res;
  
  if(FSBUF.fsidx!=fs->idx && FSBUF.dirty) {         /* Are we owner of this buf? */
    res= move_window (FatFs[FSBUF.fsidx],&FSBUF,0); 
    if(!res)
      return res;
    FSBUF.sect=0;
  }
#endif
  return move_window(fs,&FSBUF,sector);
}




static
BOOL move_fp_window(
  FIL* fp,
  DWORD  sector
)
{
#if _USE_FS_BUF == 0
  return move_window(fp->fs,&FPBUF,sector);
#else
  return move_fs_window(fp->fs,sector);
#endif
}




/*-----------------------------------------------------------------------*/
/* Clean-up cached data                                                  */
/*-----------------------------------------------------------------------*/

#if !_FS_READONLY
static
FRESULT sync (          /* FR_OK: successful, FR_RW_ERROR: failed */
  FATFS *fs             /* File system object */
)
{
  FSBUF.dirty = TRUE;
  if (!move_fs_window(fs, 0)) return FR_RW_ERROR;
#if _USE_FSINFO
  /* Update FSInfo sector if needed */
  if (fs->fs_type == FS_FAT32 && fs->fsi_flag) {
    FSBUF.sect = 0;
    memset(FSBUF.data, 0, 512);
    ST_WORD(&FSBUF.data[BS_55AA], 0xAA55);
    ST_DWORD(&FSBUF.data[FSI_LeadSig], 0x41615252);
    ST_DWORD(&FSBUF.data[FSI_StrucSig], 0x61417272);
    ST_DWORD(&FSBUF.data[FSI_Free_Count], fs->free_clust);
    ST_DWORD(&FSBUF.data[FSI_Nxt_Free], fs->last_clust);
    disk_write(fs->drive, FSBUF.data, fs->fsi_sector, 1);
    fs->fsi_flag = 0;
  }
#endif
  /* Make sure that no pending write process in the physical drive */
  if (disk_ioctl(fs->drive, CTRL_SYNC, NULL) != RES_OK) return FR_RW_ERROR;
  return FR_OK;
}
#endif




/*-----------------------------------------------------------------------*/
/* Get a cluster status                                                  */
/*-----------------------------------------------------------------------*/

static
DWORD get_cluster (     /* 0,>=2: successful, 1: failed */
  FATFS *fs,            /* File system object */
  DWORD clust           /* Cluster# to get the link information */
)
{
  WORD wc, bc;
  DWORD fatsect;


  if (clust >= 2 && clust < fs->max_clust) {        /* Valid cluster# */
    fatsect = fs->fatbase;
    switch (fs->fs_type) {
    case FS_FAT12 :
      bc = (WORD)clust * 3 / 2;
      if (!move_fs_window(fs, fatsect + (bc / S_SIZ))) break;
      wc = FSBUF.data[bc & (S_SIZ - 1)]; bc++;
      if (!move_fs_window(fs, fatsect + (bc / S_SIZ))) break;
      wc |= (WORD)FSBUF.data[bc & (S_SIZ - 1)] << 8;
      return (clust & 1) ? (wc >> 4) : (wc & 0xFFF);

    case FS_FAT16 :
      if (!move_fs_window(fs, fatsect + (clust / (S_SIZ / 2)))) break;
      return LD_WORD(&FSBUF.data[((WORD)clust * 2) & (S_SIZ - 1)]);

    case FS_FAT32 :
      if (!move_fs_window(fs, fatsect + (clust / (S_SIZ / 4)))) break;
      return LD_DWORD(&FSBUF.data[((WORD)clust * 4) & (S_SIZ - 1)]) & 0x0FFFFFFF;
    }
  }

  return 1; /* There is no cluster information, or an error occured */
}




/*-----------------------------------------------------------------------*/
/* Change a cluster status                                               */
/*-----------------------------------------------------------------------*/

#if !_FS_READONLY
static
BOOL put_cluster (      /* TRUE: successful, FALSE: failed */
  FATFS *fs,            /* File system object */
  DWORD clust,          /* Cluster# to change */
  DWORD val             /* New value to mark the cluster */
)
{
  WORD bc;
  BYTE *p;
  DWORD fatsect;


  fatsect = fs->fatbase;
  switch (fs->fs_type) {
  case FS_FAT12 :
    bc = (WORD)clust * 3 / 2;
    if (!move_fs_window(fs, fatsect + (bc / S_SIZ))) return FALSE;
    p = &FSBUF.data[bc & (S_SIZ - 1)];
    *p = (clust & 1) ? ((*p & 0x0F) | ((BYTE)val << 4)) : (BYTE)val;
    bc++;
    FSBUF.dirty = TRUE; 
    if (!move_fs_window(fs, fatsect + (bc / S_SIZ))) return FALSE;
    p = &FSBUF.data[bc & (S_SIZ - 1)];
    *p = (clust & 1) ? (BYTE)(val >> 4) : ((*p & 0xF0) | ((BYTE)(val >> 8) & 0x0F));
    break;

  case FS_FAT16 :
    if (!move_fs_window(fs, fatsect + (clust / (S_SIZ / 2)))) return FALSE;
    ST_WORD(&FSBUF.data[((WORD)clust * 2) & (S_SIZ - 1)], (WORD)val);
    break;

  case FS_FAT32 :
    if (!move_fs_window(fs, fatsect + (clust / (S_SIZ / 4)))) return FALSE;
    ST_DWORD(&FSBUF.data[((WORD)clust * 4) & (S_SIZ - 1)], val);
    break;

  default :
    return FALSE;
  }
  FSBUF.dirty = TRUE;
  return TRUE;
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Remove a cluster chain                                                */
/*-----------------------------------------------------------------------*/

#if !_FS_READONLY
static
BOOL remove_chain (     /* TRUE: successful, FALSE: failed */
  FATFS *fs,            /* File system object */
  DWORD clust           /* Cluster# to remove chain from */
)
{
  DWORD nxt;


  while (clust >= 2 && clust < fs->max_clust) {
    nxt = get_cluster(fs, clust);
    if (nxt == 1) return FALSE;
    if (!put_cluster(fs, clust, 0)) return FALSE;
    if (fs->free_clust != 0xFFFFFFFF) {
      fs->free_clust++;
#if _USE_FSINFO
      fs->fsi_flag = 1;
#endif
    }
    clust = nxt;
  }
  return TRUE;
}
#endif




/*-----------------------------------------------------------------------*/
/* Stretch or create a cluster chain                                     */
/*-----------------------------------------------------------------------*/

#if !_FS_READONLY
static
DWORD create_chain (    /* 0: no free cluster, 1: error, >=2: new cluster number */
  FATFS *fs,            /* File system object */
  DWORD clust           /* Cluster# to stretch, 0 means create new */
)
{
  DWORD cstat, ncl, scl, mcl = fs->max_clust;


  if (clust == 0) {                       /* Create new chain */
    scl = fs->last_clust;                 /* Get suggested start point */
    if (scl == 0 || scl >= mcl) scl = 1;
  }
  else {                                  /* Stretch existing chain */
    cstat = get_cluster(fs, clust);       /* Check the cluster status */
    if (cstat < 2) return 1;              /* It is an invalid cluster */
    if (cstat < mcl) return cstat;        /* It is already followed by next cluster */
    scl = clust;
  }

  ncl = scl;                              /* Start cluster */
  for (;;) {
    ncl++;                                /* Next cluster */
    if (ncl >= mcl) {                     /* Wrap around */
      ncl = 2;
      if (ncl > scl) return 0;            /* No free custer */
    }
    cstat = get_cluster(fs, ncl);         /* Get the cluster status */
    if (cstat == 0) break;                /* Found a free cluster */
    if (cstat == 1) return 1;             /* Any error occured */
    if (ncl == scl) return 0;             /* No free custer */
  }

  if (!put_cluster(fs, ncl, 0x0FFFFFFF)) return 1;      /* Mark the new cluster "in use" */
  if (clust && !put_cluster(fs, clust, ncl)) return 1;  /* Link it to previous one if needed */

  fs->last_clust = ncl;                   /* Update fsinfo */
  if (fs->free_clust != 0xFFFFFFFF) {
    fs->free_clust--;
#if _USE_FSINFO
    fs->fsi_flag = 1;
#endif
  }

  return ncl;   /* Return new cluster number */
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Get sector# from cluster#                                             */
/*-----------------------------------------------------------------------*/

static
DWORD clust2sect (      /* !=0: sector number, 0: failed - invalid cluster# */
  FATFS *fs,            /* File system object */
  DWORD clust           /* Cluster# to be converted */
)
{
  clust -= 2;
  if (clust >= (fs->max_clust - 2)) return 0;       /* Invalid cluster# */
  return clust * fs->sects_clust + fs->database;
}




/*-----------------------------------------------------------------------*/
/* Move directory pointer to next                                        */
/*-----------------------------------------------------------------------*/

static
BOOL next_dir_entry (   /* TRUE: successful, FALSE: could not move next */
  DIR *dirobj           /* Pointer to directory object */
)
{
  DWORD clust;
  WORD idx;
  FATFS *fs = dirobj->fs;


  idx = dirobj->index + 1;
  if ((idx & ((S_SIZ - 1) / 32)) == 0) {            /* Table sector changed? */
    dirobj->sect++;                                 /* Next sector */
    if (!dirobj->clust) {                           /* In static table */
      if (idx >= fs->n_rootdir) return FALSE;       /* Reached to end of table */
    } else {                                        /* In dynamic table */
      if (((idx / (S_SIZ / 32)) & (fs->sects_clust - 1)) == 0) {  /* Cluster changed? */
        clust = get_cluster(fs, dirobj->clust);     /* Get next cluster */
        if (clust < 2 || clust >= fs->max_clust)    /* Reached to end of table */
          return FALSE;
        dirobj->clust = clust;                      /* Initialize for new cluster */
        dirobj->sect = clust2sect(fs, clust);
      }
    }
  }
  dirobj->index = idx;  /* Lower 4 bit of dirobj->index indicates offset in dirobj->sect */
  return TRUE;
}




/*-----------------------------------------------------------------------*/
/* Get file status from directory entry                                  */
/*-----------------------------------------------------------------------*/

#if _FS_MINIMIZE <= 1
static
void get_fileinfo (     /* No return code */
  FILINFO *finfo,       /* Ptr to store the file information */
  const BYTE *dir       /* Ptr to the directory entry */
)
{
  BYTE n, c, a;
  char *p;


  p = &finfo->fname[0];
  a = _USE_NTFLAG ? dir[DIR_NTres] : 0;   /* NT flag */
  for (n = 0; n < 8; n++) {   /* Convert file name (body) */
    c = dir[n];
    if (c == ' ') break;
    if (c == 0x05) c = 0xE5;
    if (a & 0x08 && c >= 'A' && c <= 'Z') c += 0x20;
    *p++ = c;
  }
  if (dir[8] != ' ') {        /* Convert file name (extension) */
    *p++ = '.';
    for (n = 8; n < 11; n++) {
      c = dir[n];
      if (c == ' ') break;
      if (a & 0x10 && c >= 'A' && c <= 'Z') c += 0x20;
      *p++ = c;
    }
  }
  *p = '\0';
  
  finfo->fattrib = dir[DIR_Attr];               /* Attribute */
  finfo->fsize = LD_DWORD(&dir[DIR_FileSize]);  /* Size */
  finfo->fdate = LD_WORD(&dir[DIR_WrtDate]);    /* Date */
  finfo->ftime = LD_WORD(&dir[DIR_WrtTime]);    /* Time */
}
#endif /* _FS_MINIMIZE <= 1 */




/*-----------------------------------------------------------------------*/
/* Pick a paragraph and create the name in format of directory entry     */
/*-----------------------------------------------------------------------*/

static
char make_dirfile (   /* 1: error - detected an invalid format, '\0'or'/': next character */
  const char **path,  /* Pointer to the file path pointer */
  char *dirname,      /* Pointer to directory name buffer {Name(8), Ext(3), NT flag(1)} */
  BOOL* lfn           /* is this an LFN name? */
)
{
  BYTE n, t, c, a, b;


  *lfn=FALSE;
  memset(dirname, ' ', 8+3);  /* Fill buffer with spaces */
  a = 0; b = 0x18;            /* NT flag */
  n = 0; t = 8;
  for (;;) {
    //printf("Path=%s\n",*path);
    c = *(*path)++;
    if (c == '\0' || c == '/') {           /* Reached to end of str or directory separator */
      if (n == 0) break;
      dirname[11] = _USE_NTFLAG ? (a & b) : 0;
      return c;
    }
    if (c < ' ' || c == 0x7F) break;       /* Reject invisible chars */
    if (c == ' ') goto md_l3;
    if (c == '.') {
#if _USE_CHDIR != 0
      if (n == 0 || (n == 1 && (*(*path)-1) == '.')) {
        goto md_l2;
      }
#endif
      if (!(a & 1) && n >= 1 && n <= 8) {  /* Enter extension part */
        n = 8; t = 11; continue;
      }
      goto md_l3;
    }
    if (_USE_SJIS &&
      ((c >= 0x81 && c <= 0x9F) ||         /* Accept S-JIS code */
        (c >= 0xE0 && c <= 0xFC))) {
      if (n == 0 && c == 0xE5)             /* Change heading \xE5 to \x05 */
        c = 0x05;
      a ^= 1; goto md_l2;
    }
    if (c == '"') break;                   /* Reject " */
    if (c <= ')') goto md_l1;              /* Accept ! # $ % & ' ( ) */
    if (c <= ',') goto md_l3;              /* Reject * + , */
    if (c <= '9') goto md_l1;              /* Accept - 0-9 */
    if (c <= '?') goto md_l3;              /* Reject : ; < = > ? */
    if (!(a & 1)) {     /* These checks are not applied to S-JIS 2nd byte */
      if (c == '|') goto md_l3;            /* Reject | */
      if (c >= '[' && c <= ']') goto md_l3;/* Reject [ \ ] */
      if (_USE_NTFLAG && c >= 'A' && c <= 'Z')
        (t == 8) ? (b &= ~0x08) : (b &= ~0x10);
      if (c >= 'a' && c <= 'z') {          /* Convert to upper case */
        c -= 0x20;
        if (_USE_NTFLAG) (t == 8) ? (a |= 0x08) : (a |= 0x10);
      }
    }
  md_l1:
    a &= ~1;
  md_l2:
    if (n >= t) goto md_l3;
    dirname[n++] = c;
  }
  return 1;
  md_l3:
#if _USE_LFN != 0
  do {
    if(c== '\\' || c==':' || c=='*' || c=='?' || c == '"' || c == '<' || c == '>' || c=='|')
      return 1;
    c = *(*path)++;
	//printf("-Path=%s\n",*path);
  } while (c != '\0' && c != '/');     /* Reached to end of str or directory separator */
  *lfn=TRUE;
  return c;
#else
  return 1;
#endif
}




/*-----------------------------------------------------------------------*/
/* Trace a file path                                                     */
/*-----------------------------------------------------------------------*/

static
FRESULT trace_path (    /* FR_OK(0): successful, !=0: error code */
  DIR *dirobj,          /* Pointer to directory object to return last directory */
  char *fn,             /* Pointer to last segment name to return {file(8),ext(3),attr(1)} */
  const char *path,     /* Full-path string to trace a file or directory */
  BYTE **dir            /* Directory pointer in Win[] to retutn */
#if _USE_LFN != 0
  ,DIR *fileobj,        /* Pointer DIR holding the beginning of the LFN, identical to dirobj if none */
  const char** spath,   /* path to start of last item's name */
  UINT *len             /* length of LFN entry, 0=non-LFN */
#endif
)
{
  DWORD clust;
  char ds;
  BYTE *dptr = NULL;
  FATFS *fs = dirobj->fs; /* Get logical drive from the given DIR structure */
  BOOL lfn;
#if _USE_LFN != 0
  BYTE a,b,i,j;
  BOOL match;
  UINT l;
  BOOL store=TRUE;
#endif

  /* Initialize directory object */
#if _USE_CHDIR != 0
  if(fs->curr_dir==0 || (*path!=0 && path[0]=='/')) {
#endif
	clust = fs->dirbase;
	if (fs->fs_type == FS_FAT32) {
	  dirobj->clust = dirobj->sclust = clust;
	  dirobj->sect = clust2sect(fs, clust);
	} else {
	  dirobj->clust = dirobj->sclust = 0;
	  dirobj->sect = clust;
	}
#if _USE_CHDIR != 0
  } else {
    clust=fs->curr_dir;
    dirobj->clust = dirobj->sclust = clust;
    dirobj->sect = clust2sect(fs, clust);
  }
#endif
  dirobj->index = 0;

#if _USE_LFN != 0
  fileobj->id=dirobj->id;
  fileobj->fs=dirobj->fs;
#endif    

#if _USE_CHDIR != 0
  while (path[0] == '/') path++;
#endif

  if (*path == '\0') {          /* Null path means the root directory */
    *dir = NULL; return FR_OK;  
  }

  for (;;) {
#if _USE_LFN != 0
    *spath=path;     // save this off, as we may need it for the LFN
    match=TRUE;
    l=0;
#endif    
	ds = make_dirfile(&path, fn, &lfn);     /* Get a paragraph into fn[] */
	//printf("Path='%s',Spath='%s', lfn=%d\n",path,*spath,lfn);
#if _USE_LFN != 0
	if(lfn)
	  *len=path-*spath-1; /* this might not be ANSI-compatible, not sure */
	else
	  *len=0;
#endif    
	if (ds == 1) return FR_INVALID_NAME;
	for (;;) {
	  if (!move_fs_window(fs, dirobj->sect)) return FR_RW_ERROR;
#if _USE_LFN != 0
	  if (store) {
		fileobj->clust = dirobj->clust;
		fileobj->sect  = dirobj->sect;
		fileobj->index = dirobj->index;
		store = FALSE;
		if (!lfn)
		  /* We don't know the length of the LFN, estimate it */
		  *len = 0;
	  }
#endif
	  dptr = &FSBUF.data[(dirobj->index & ((S_SIZ - 1) / 32)) * 32];  /* Pointer to the directory entry */
	  if (dptr[DIR_Name] == 0)            /* Has it reached to end of dir? */
		return !ds ? FR_NO_FILE : FR_NO_PATH;
#if _USE_LFN != 0
	  if (dptr[DIR_Name] != 0xE5) {            /* Matched? */
		//trace(dptr,0,32);
		if((dptr[DIR_Attr] & AM_LFN) == AM_LFN) {
		  if (lfn) {
			//printf("Hey, here's an LFN, match=%d\n",match);
			i=((dptr[0]&0x1f)-1)*13;
			j=0;
			while(j<13 && match) {
			  a=dptr[pgm_read_byte(LFN_pos+j)];
			  b=dptr[pgm_read_byte(LFN_pos+j++)+1];
			  //printf("a=%x, b=%x, spath[i] = %x\n",a,b,(*spath)[i]);
			  if(!a && !b) {
				j--;
				break;
			  }
				if(a!=(*spath)[i++]) {
				  match=FALSE;
				}
			}
			l+=j;
		  } else {
			/* Track the length of the LFN entry in case it belongs to our file */
			*len = *len + 13;
		  }
		} else if (!(dptr[DIR_Attr] & AM_VOL)) {  // we're a normal entry
		  if (lfn) {
			//printf("len=%d=%d\n",l,(path-(*spath)-1));
			if(lfn && (match && l == *len)) {// match
			  //printf("found a match, len=%d\n",l);
				memcpy(fn,&dptr[DIR_Name], 8+3);
				//printf("found a match, len=%d\n",*len);
				fn[11] = dptr[DIR_NTres];
				break;
			}
			l=0;
			match=TRUE;
		  } else {
			/* No LFN to match against */
			if (dptr[DIR_Name] != 0xE5            /* Matched? */
				&& !(dptr[DIR_Attr] & AM_VOL)
				&& !memcmp(&dptr[DIR_Name], fn, 8+3) ) {
			  break;
			}
		  }
		  store = TRUE;
		}
	  } else
		/* This is a deleted entry, move fileobj forward */
		store = TRUE;
#else
	  if (dptr[DIR_Name] != 0xE5                  /* Matched? */
		  && !(dptr[DIR_Attr] & AM_VOL)
		  && !memcmp(&dptr[DIR_Name], fn, 8+3) ) {
		break;
	  }
#endif
	  if (!next_dir_entry(dirobj)) {                    /* Next directory pointer */
		if (!lfn)
		  *len = 0;
		return !ds ? FR_NO_FILE : FR_NO_PATH;
	  }
    }
    if (!ds) { *dir = dptr; return FR_OK; }             /* Matched with end of path */
    if (!(dptr[DIR_Attr] & AM_DIR)) return FR_NO_PATH;  /* Cannot trace because it is a file */
    clust = ((DWORD)LD_WORD(&dptr[DIR_FstClusHI]) << 16)
	  | LD_WORD(&dptr[DIR_FstClusLO]);                  /* Get cluster# of the directory */
    dirobj->clust = dirobj->sclust = clust;             /* Restart scanning at the new directory */
    dirobj->sect = clust2sect(fs, clust);
    dirobj->index = 2;
  }
}




/*-----------------------------------------------------------------------*/
/* Reserve a directory entry                                             */
/*-----------------------------------------------------------------------*/

#if !_FS_READONLY
static
FRESULT reserve_direntry (  /* FR_OK: successful, FR_DENIED: no free entry, FR_RW_ERROR: a disk error occured */
  DIR *dirobj,              /* Target directory to create new entry */
  BYTE **dir                /* Pointer to pointer to created entry to retutn */
#if _USE_LFN != 0
  ,UINT len
#endif
)
{
  DWORD clust, sector;
  BYTE c, n, *dptr;
  FATFS *fs = dirobj->fs;

#if _USE_LFN != 0
  BYTE entries=0;
  WORD isave=0;
  DWORD csave=0,ssave=0;
  
  len=(len+25)/13;
#endif      
  /* Re-initialize directory object */
  clust = dirobj->sclust;
  if (clust) {          /* Dyanmic directory table */
    dirobj->clust = clust;
    dirobj->sect = clust2sect(fs, clust);
  } else {              /* Static directory table */
    dirobj->sect = fs->dirbase;
  }
  dirobj->index = 0;

  //printf("We're looking for %d entries\n",num);
  do {
    if (!move_fs_window(fs, dirobj->sect)) return FR_RW_ERROR;
    dptr = &FSBUF.data[(dirobj->index & ((S_SIZ - 1) / 32)) * 32];  /* Pointer to the directory entry */
    //printf("clust=%d,sector=%d,index=%d\n",dirobj->clust,dirobj->sect,dirobj->index);
    //trace(dptr,0,32);
    c = dptr[DIR_Name];
    //printf("This entry has first byte=%2.2x\n",c);
    if (c == 0 || c == 0xE5) {      /* Found an empty entry! */
#if _USE_LFN != 0
      /* capture initial entry. */
      if((entries++) == 0) {
        //printf("OK, this one looks good to start\n");
        *dir=dptr;
        //printf("Saved off: clust=%d,sector=%d,index=%d\n",dirobj->clust,dirobj->sect,dirobj->index);
        isave=dirobj->index;
        ssave=dirobj->sect;
        csave=dirobj->clust;
      }
      if(entries==len) {
        //printf("We found enough within the dir\n");
        dirobj->index=isave;
        dirobj->sect=ssave;
        dirobj->clust=csave;
        //printf("Restored: clust=%d,sector=%d,index=%d\n",dirobj->clust,dirobj->sect,dirobj->index);
        return FR_OK;
      }
    } else if(entries!=len){
      //printf("Darn, not enough entries\n");
      entries=0;
#else      
      *dir = dptr; return FR_OK;
#endif
	}
  } while (next_dir_entry(dirobj));                 /* Next directory pointer */
  /* Reached to end of the directory table */
  /* Abort when static table or could not stretch dynamic table */
  if (!clust || !(clust = create_chain(fs, dirobj->clust))) return FR_DENIED;
  if (clust == 1 || !move_fs_window(fs, 0)) return FR_RW_ERROR;
  /* Cleanup the expanded table */
  FSBUF.sect = sector = clust2sect(fs, clust);
  memset(FSBUF.data, 0, S_SIZ);
  for (n = fs->sects_clust; n; n--) {
    if (disk_write(fs->drive, FSBUF.data, sector, 1) != RES_OK)
      return FR_RW_ERROR;
    sector++;
  }
  FSBUF.dirty = TRUE;
#if _USE_LFN != 0
  if(entries) {   // we had to expand the table, but now we need to go back.
    if (!move_fs_window(fs, ssave)) return FR_RW_ERROR;
    dirobj->index=isave;
    dirobj->sect=ssave;
    dirobj->clust=csave;
  //printf("Restored: clust=%d,sector=%d,index=%d\n",dirobj->clust,dirobj->sect,dirobj->index);
  } else {
	/* We allocated a new cluster for all entries, point dirobj there */
	dirobj->index = 0;
	dirobj->sect  = FSBUF.sect;
	dirobj->clust = clust;
	*dir = FSBUF.data;
  }
#else
  *dir = FSBUF.data;
#endif
  return FR_OK;
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Load boot record and check if it is a FAT boot record                 */
/*-----------------------------------------------------------------------*/

static const PROGMEM char fat32string[] = "FAT32";

static
BYTE check_fs (     /* 0:The FAT boot record, 1:Valid boot record but not an FAT, 2:Not a boot record or error */
  FATFS *fs,        /* File system object */
  DWORD sect        /* Sector# (lba) to check if it is a FAT boot record or not */
)
{
  if (!move_fs_window(fs, sect))                    /* Load boot record, save off old data in process */
    return 2;
  if(!sect && disk_read(fs->drive, FSBUF.data, sect, 1) != RES_OK)  /* Load boot record, if sector 0 */
    return 2;
  if (LD_WORD(&FSBUF.data[BS_55AA]) != 0xAA55)      /* Check record signature (always placed at offset 510 even if the sector size is >512) */
    return 2;

  if (!memcmp_P(&FSBUF.data[BS_FilSysType], fat32string, 3))        /* Check FAT signature */
    return 0;
  if (!memcmp_P(&FSBUF.data[BS_FilSysType32], fat32string, 5) && !(FSBUF.data[BPB_ExtFlags] & 0x80))
    return 0;

  return 1;
}




/*-----------------------------------------------------------------------*/
/* Make sure that the file system is valid                               */
/*-----------------------------------------------------------------------*/

static
FRESULT auto_mount (    /* FR_OK(0): successful, !=0: any error occured */
  const char **path,    /* Pointer to pointer to the path name (drive number) */
  FATFS **rfs,          /* Pointer to pointer to the found file system object */
  BYTE chk_wp           /* !=0: Check media write protection for write access */
)
{
  BYTE drv, fmt, *tbl;
  DSTATUS stat;
  DWORD bootsect, fatsize, totalsect, maxclust;
  const char *p = *path;
  FATFS *fs;


  /* Get drive number from the path name */
  while (*p == ' ') p++;              /* Strip leading spaces */
  drv = p[0] - '0';                   /* Is there a drive number? */
  if (drv <= 9 && p[1] == ':')
    p += 2;                           /* Found a drive number, get and strip it */
  else
    drv = 0;                          /* No drive number is given, use drive number 0 as default */
#if _USE_CHDIR == 0
  if (*p == '/') p++;                 /* Strip heading slash */
#endif
  *path = p;                          /* Return pointer to the path name */

  /* Check if the drive number is valid or not */
  if (drv >= _DRIVES) return FR_INVALID_DRIVE;    /* Is the drive number valid? */
  if (!(fs = FatFs[drv])) return FR_NOT_ENABLED;  /* Is the file system object registered? */
  *rfs = fs;                          /* Returen pointer to the corresponding file system object */

  /* Check if the logical drive has been mounted or not */
  if (fs->fs_type) {
    stat = disk_status(fs->drive);
    if (!(stat & STA_NOINIT)) {       /* If the physical drive is kept initialized */
#if !_FS_READONLY
      if (chk_wp && (stat & STA_PROTECT))         /* Check write protection if needed */
        return FR_WRITE_PROTECTED;
#endif
      return FR_OK;                   /* The file system object is valid */
    }
  }

  /* The logical drive has not been mounted, following code attempts to mount the logical drive */

  memset(fs, 0, sizeof(FATFS));       /* Clean-up the file system object */
#if _USE_1_BUF != 0
  fs->idx=drv;
#endif
  fs->drive = LD2PD(drv);             /* Bind the logical drive and a physical drive */
  stat = disk_initialize(fs->drive);  /* Initialize low level disk I/O layer */
  if (stat & STA_NOINIT)              /* Check if the drive is ready */
    return FR_NOT_READY;
#if S_MAX_SIZ > 512                  /* Check disk sector size */
  if (disk_ioctl(drv, GET_SECTOR_SIZE, &S_SIZ) != RES_OK || S_SIZ > S_MAX_SIZ)
    return FR_NO_FILESYSTEM;
#endif
#if !_FS_READONLY
  if (chk_wp && (stat & STA_PROTECT)) /* Check write protection if needed */
    return FR_WRITE_PROTECTED;
#endif
  /* Search FAT partition on the drive */
  fmt = check_fs(fs, bootsect = 0);   /* Check sector 0 as an SFD format */
  if (fmt == 1) {                     /* Not a FAT boot record, it may be patitioned */
    /* Check a partition listed in top of the partition table */
    tbl = &FSBUF.data[MBR_Table + LD2PT(drv) * 16]; /* Partition table */
    if (tbl[4]) {                     /* Is the partition existing? */
      bootsect = LD_DWORD(&tbl[8]);   /* Partition offset in LBA */
      fmt = check_fs(fs, bootsect);   /* Check the partition */
    }
  }
  if (fmt || LD_WORD(&FSBUF.data[BPB_BytsPerSec]) != S_SIZ) /* No valid FAT patition is found */
    return FR_NO_FILESYSTEM;

  /* Initialize the file system object */
  fatsize = LD_WORD(&FSBUF.data[BPB_FATSz16]);      /* Number of sectors per FAT */
  if (!fatsize) fatsize = LD_DWORD(&FSBUF.data[BPB_FATSz32]);
  fs->sects_fat = fatsize;
  fs->n_fats = FSBUF.data[BPB_NumFATs];             /* Number of FAT copies */
  fatsize *= fs->n_fats;                            /* (Number of sectors in FAT area) */
  fs->fatbase = bootsect + LD_WORD(&FSBUF.data[BPB_RsvdSecCnt]); /* FAT start sector (lba) */
  fs->sects_clust = FSBUF.data[BPB_SecPerClus];     /* Number of sectors per cluster */
  fs->n_rootdir = LD_WORD(&FSBUF.data[BPB_RootEntCnt]); /* Nmuber of root directory entries */
  totalsect = LD_WORD(&FSBUF.data[BPB_TotSec16]);   /* Number of sectors on the file system */
  if (!totalsect) totalsect = LD_DWORD(&FSBUF.data[BPB_TotSec32]);
  fs->max_clust = maxclust = (totalsect             /* Last cluster# + 1 */
    - LD_WORD(&FSBUF.data[BPB_RsvdSecCnt]) - fatsize - fs->n_rootdir / (S_SIZ/32)
    ) / fs->sects_clust + 2;

  fmt = FS_FAT12;                     /* Determine the FAT sub type */
  if (maxclust >= 0xFF7) fmt = FS_FAT16;
  if (maxclust >= 0xFFF7) fmt = FS_FAT32;
  fs->fs_type = fmt;

  if (fmt == FS_FAT32)
    fs->dirbase = LD_DWORD(&FSBUF.data[BPB_RootClus]);  /* Root directory start cluster */
  else
    fs->dirbase = fs->fatbase + fatsize;            /* Root directory start sector (lba) */
  fs->database = fs->fatbase + fatsize + fs->n_rootdir / (S_SIZ/32);  /* Data start sector (lba) */

#if !_FS_READONLY
  fs->free_clust = 0xFFFFFFFF;
# if _USE_FSINFO
  /* Load fsinfo sector if needed */
  if (fmt == FS_FAT32) {
    fs->fsi_sector = bootsect + LD_WORD(&FSBUF.data[BPB_FSInfo]);
    //if (disk_read(fs->drive, FSBUF.data, fs->fsi_sector, 1) == RES_OK &&
    if (!move_fs_window(fs,fs->fsi_sector) &&
      LD_WORD(&FSBUF.data[BS_55AA]) == 0xAA55 &&
      LD_DWORD(&FSBUF.data[FSI_LeadSig]) == 0x41615252 &&
      LD_DWORD(&FSBUF.data[FSI_StrucSig]) == 0x61417272) {
      fs->last_clust = LD_DWORD(&FSBUF.data[FSI_Nxt_Free]);
      fs->free_clust = LD_DWORD(&FSBUF.data[FSI_Free_Count]);
    }
  }
# endif
#endif
  fs->id = ++fsid;                    /* File system mount ID */
  return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Check if the file/dir object is valid or not                          */
/*-----------------------------------------------------------------------*/

static
FRESULT validate (    /* FR_OK(0): The object is valid, !=0: Not valid */
  const FATFS *fs,    /* Pointer to the file system object */
  WORD id             /* id member of the target object to be checked */
)
{
  if (!fs || fs->id != id)
    return FR_INVALID_OBJECT;
  if (disk_status(fs->drive) & STA_NOINIT)
    return FR_NOT_READY;

  return FR_OK;
}




#if _USE_LFN != 0
static
BYTE compute_checksum(unsigned char *buf) {
  BYTE i,rc;
  rc=buf[0];
  for(i=1;i<11;i++) {
    rc=(rc&1?(rc>>1)| 0x80:rc>>1);
    rc+=buf[i];
  }
  return rc;
}




static
BYTE dos_char(
    BYTE x
)
{
  if(x>('a'-1) && x<('z'+1))
    x=x-'a'+'A';
  else if(x>(' '-1) && x<'+') {
  } else if(x>('A'-1) && x<('Z'+1)) {
  } else if(x>('0'-1) && x<('9'+1)) {
  } else if(x&0x80) {
  } else switch(x) {
    case ',':
    case '-':
    case '^':
    case '~':
    case '=':
    case '_':
    case '`':
    case '{':
    case '}':
      break;
    default:
      x='_';
      break;
  }
  return x;
}




static
void create_short_name(
  const char* name, 
  UINT len, 
  char* buf
)
{
  BYTE i=0,k,l=0;
  BYTE j=len;
  
  memset(buf,' ',11);
  buf[11]=0;
  if(name[0]!='.') {
    while(--j) { /* find the last '.' */
      if(name[j]=='.') break;
    }
    k=j+1;
    i=8;
    if(k>1) {
      while(k<len && i<11) {
        if(name[k]!=' ') {
          // build extension
          buf[i++]=dos_char(name[k++]);
        }
      }
	} else
	  j=6;
  }
  i=0; /* now build start */
  while(i<6 && l<j) {
    if(name[l]!=' ' && name[l]!='.') {
      buf[i++]=dos_char(name[l]);
    }
    l++;
  }
  if(i<3) {
    buf[i++]='A'; /* FIXME: replace this with a way to generate a random or hash hex word. */
    buf[i++]='B';
    buf[i++]='C';
    buf[i++]='D';
  }
  buf[i++]='~';
  buf[i++]='1';
  if(((BYTE)buf[0])==0xe5)
    buf[0]=0x05;
}




static
BOOL fix_short_name(
  char *buf
)
{
  BYTE i=8;
  BOOL c=FALSE;
  while(buf[--i]==' ');
  do {
    if(buf[i]=='9') {
      buf[i]='0';
      c=TRUE;
    } else if(c && buf[i]=='~') {
        buf[i]='1';
        break;
    } else {
      buf[i]++;
      c=FALSE;
      break;
    }
  } while (--i);
  if(c) {
    if(!i) return FALSE;
    buf[i-1]='~';
  }
  return TRUE;
}




static
FRESULT chk_filename( /* FR_EXIST means name is taken. */
  DIR *dirobj,        /* Target directory to create new entry */
  char* fn            /* name to check */
)
{
  BYTE *dptr;
  FATFS *fs = dirobj->fs;
  DWORD clust;
  
  /* Re-initialize directory object */
  clust = dirobj->sclust;
  if (clust) {          /* Dyanmic directory table */
    dirobj->clust = clust;
    dirobj->sect = clust2sect(fs, clust);
  } else {              /* Static directory table */
    dirobj->sect = fs->dirbase;
  }
  dirobj->index = 0;

  do {
    if (!move_fs_window(fs, dirobj->sect)) return FR_RW_ERROR;
    dptr = &FSBUF.data[(dirobj->index & ((S_SIZ - 1) / 32)) * 32]; /* Pointer to the directory entry */
    //trace(dptr,0,32);
    if(dptr[DIR_Name] == 0) {         /* if we got here, we have a match */
      return FR_OK;
    } else if(*dptr!=0xe5 
			  && (dptr[DIR_Attr] & AM_LFN) != AM_LFN
			  && !(dptr[DIR_Attr] & AM_VOL)
			  && !memcmp(&dptr[DIR_Name], fn, 8+3) )  /* start over. */
	  return FR_EXIST;
  } while (next_dir_entry(dirobj));   /* Next directory pointer */
  /* Reached to end of the directory table */
  return FR_OK;
}
  
FRESULT add_direntry(
  DIR *dirobj,          /* Target directory to create new entry */
  BYTE **dir,           /* pointer to created entry */
  const char* spath,
  UINT len,
  char* fn
)
{
  FATFS *fs = dirobj->fs;
  DWORD clust,sect;
  WORD index;
  BYTE entries,i,j,k;
  BYTE chk;

  entries=i=(len+12)/13;
  
  fn[11]=0;             /* ALL uppercase */

  clust=dirobj->clust;  /* save off entries needed. */
  index=dirobj->index;
  sect=dirobj->sect;
  create_short_name(spath,len,fn);
  for(;;) {
    if(!chk_filename(dirobj,fn)) break;
    if(!fix_short_name(fn))
      return FR_EXIST;
  }
  chk=compute_checksum((unsigned char*)fn);
  dirobj->clust=clust;  /* we now have a good name, use it */
  dirobj->index=index;
  dirobj->sect=sect;
  //printf("We are using shortname=%s\n",fn);
  while(move_fs_window(fs, dirobj->sect)) {
    *dir = &FSBUF.data[(dirobj->index & ((S_SIZ - 1) / 32)) * 32]; /* Pointer to the directory entry */
    if(!i)
	  return FR_OK;
    FSBUF.dirty = TRUE;
    memset(*dir, 0, 32);              /* Initialize the new entry with open name */
    (*dir)[DIR_Name]=(i==entries?0x40:0) | (i);
    j=0;
    i--;
    k=(i)*13;
    while(j<13 && k<len) {
      (*dir)[pgm_read_byte(LFN_pos+j++)]=spath[k++];
    }
    j++;
    while(j<13) {
      (*dir)[pgm_read_byte(LFN_pos+j)]=0xff;
      (*dir)[pgm_read_byte(LFN_pos+j++)+1]=0xff;
    }
    (*dir)[DIR_Attr]=AM_LFN;
    (*dir)[DIR_Chksum]=chk;
    if(!next_dir_entry(dirobj)) break;
  }
  return FR_RW_ERROR;
}
#endif




/*--------------------------------------------------------------------------

   Public Functions

--------------------------------------------------------------------------*/



/*-----------------------------------------------------------------------*/
/* Mount/Unmount a Locical Drive                                         */
/*-----------------------------------------------------------------------*/

FRESULT f_mount (
  BYTE drv,   /* Logical drive number to be mounted/unmounted */
  FATFS *fs   /* Pointer to new file system object (NULL for unmount)*/
)
{
  FATFS *fsobj;


  if (drv >= _DRIVES) return FR_INVALID_DRIVE;
  fsobj = FatFs[drv];
  FatFs[drv] = fs;
  if (fsobj) memset(fsobj, 0, sizeof(FATFS));
  if (fs) memset(fs, 0, sizeof(FATFS));

  return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Open or Create a File                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_open (
  FIL *fp,            /* Pointer to the blank file object */
  const char *path,   /* Pointer to the file name */
  BYTE mode           /* Access mode and file open mode flags */
)
{
  FRESULT res;
  BYTE *dir;
  DIR dirobj;
  char fn[8+3+1];
  FATFS *fs;
#if _USE_LFN != 0
  UINT len;
  DIR fileobj;
  const char* spath;
#endif


#if _USE_FS_BUF == 0 
  FPBUF.dirty=FALSE;
#endif
  fp->fs = NULL;
#if !_FS_READONLY
  mode &= (FA_READ|FA_WRITE|FA_CREATE_ALWAYS|FA_OPEN_ALWAYS|FA_CREATE_NEW);
  res = auto_mount(&path, &fs, (BYTE)(mode & (FA_WRITE|FA_CREATE_ALWAYS|FA_OPEN_ALWAYS|FA_CREATE_NEW)));
#else
  mode &= FA_READ;
  res = auto_mount(&path, &fs, 0);
#endif
  if (res != FR_OK) return res;
  dirobj.fs = fs;

  /* Trace the file path */
  //printf("getting ready to trace file path\n");

#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
  res = trace_path(&dirobj, fn, path, &dir);   /* trace the file path */
#endif
#if !_FS_READONLY
  /* Create or Open a file */
# if _USE_CHDIR != 0
#  if _USE_LFN != 0
  if((len==1 && spath[0]=='.') || (len==2 && spath[0]=='.' && spath[1]=='.'))
    return FR_INVALID_NAME;
  else
#  endif
  if(fn[0]=='.')
    return FR_INVALID_NAME;
# endif
  if (mode & (FA_CREATE_ALWAYS|FA_OPEN_ALWAYS|FA_CREATE_NEW)) {
    DWORD ps, rs;
    if (res != FR_OK) {               /* No file, create new */
      if (res != FR_NO_FILE) return res;
# if _USE_LFN != 0
      res = reserve_direntry(&dirobj, &dir,len);
      if (res != FR_OK) return res;
      if(len && add_direntry(&dirobj,&dir,spath,len,fn)) return FR_RW_ERROR;
# else
      res = reserve_direntry(&dirobj, &dir);
      if (res != FR_OK) return res;
# endif
      memset(dir, 0, 32);             /* Initialize the new entry with open name */
      memcpy(&dir[DIR_Name], fn, 8+3);
      dir[DIR_NTres] = fn[11];
      mode |= FA_CREATE_ALWAYS;
    }
    else {                            /* Any object is already existing */
      if (mode & FA_CREATE_NEW)       /* Cannot create new */
        return FR_EXIST;
      if (dir == NULL)                /* Cannot overwrite it */
        return FR_DENIED;
      if (dir[DIR_Attr] & AM_DIR)
	return FR_IS_DIRECTORY;
      if (dir[DIR_Attr] & AM_RDO)
	return FR_IS_READONLY;
      if (mode & FA_CREATE_ALWAYS) {      /* Resize it to zero if needed */
        rs = ((DWORD)LD_WORD(&dir[DIR_FstClusHI]) << 16) | LD_WORD(&dir[DIR_FstClusLO]);  /* Get start cluster */
        ST_WORD(&dir[DIR_FstClusHI], 0);  /* cluster = 0 */
        ST_WORD(&dir[DIR_FstClusLO], 0);
        ST_DWORD(&dir[DIR_FileSize], 0);  /* size = 0 */
        FSBUF.dirty = TRUE;
        ps = FSBUF.sect;                  /* Remove the cluster chain */
        if (!remove_chain(fs, rs) || !move_fs_window(fs, ps))
          return FR_RW_ERROR;
        fs->last_clust = rs - 1;          /* Reuse the cluster hole */
      }
    }
    if (mode & FA_CREATE_ALWAYS) {
      dir[DIR_Attr] = AM_ARC;             /* New attribute */
      ps = get_fattime();
      ST_DWORD(&dir[DIR_WrtTime], ps);    /* Updated time */
      ST_DWORD(&dir[DIR_CrtTime], ps);    /* Created time */
      sync(fs);
    }
  }
  /* Open an existing file */
  else {
#endif /* !_FS_READONLY */
    if (res != FR_OK) return res;     /* Trace failed */
    if (dir == NULL || (dir[DIR_Attr] & AM_DIR))       /* It is a directory */
      return FR_NO_FILE;
#if !_FS_READONLY
    if ((mode & FA_WRITE) && (dir[DIR_Attr] & AM_RDO)) /* R/O violation */
      return FR_DENIED;
  }

  fp->dir_sect = FSBUF.sect;          /* Pointer to the directory entry */
  fp->dir_ptr = dir;
#endif
  fp->flag = mode;                    /* File access mode */
  fp->org_clust =                     /* File start cluster */
    ((DWORD)LD_WORD(&dir[DIR_FstClusHI]) << 16) | LD_WORD(&dir[DIR_FstClusLO]);
  fp->fsize = LD_DWORD(&dir[DIR_FileSize]);         /* File size */
  fp->fptr = 0;                                     /* File ptr */
  fp->sect_clust = 1;                               /* Sector counter */
  fp->fs = fs; fp->id = fs->id;       /* Owner file system object of the file */

  return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Read File                                                             */
/*-----------------------------------------------------------------------*/

FRESULT f_read (
  FIL *fp,      /* Pointer to the file object */
  void *buff,   /* Pointer to data buffer */
  UINT btr,     /* Number of bytes to read */
  UINT *br      /* Pointer to number of bytes read */
)
{
  DWORD clust, sect, remain;
  UINT rcnt, cc;
  BYTE *rbuff = buff;
  FRESULT res;
  FATFS *fs = fp->fs;


  *br = 0;
  res = validate(fs, fp->id);                   /* Check validity of the object */
  if (res != FR_OK) return res;
  if (fp->flag & FA__ERROR) return FR_RW_ERROR; /* Check error flag */
  if (!(fp->flag & FA_READ)) return FR_DENIED;  /* Check access mode */
  remain = fp->fsize - fp->fptr;
  if (btr > remain) btr = (UINT)remain;         /* Truncate read count by number of bytes left */

  for ( ;  btr;                                 /* Repeat until all data transferred */
    rbuff += rcnt, fp->fptr += rcnt, *br += rcnt, btr -= rcnt) {
    //printf("Sect=%d, curr_sect=%d\n",sect,fp->curr_sect);
    if ((fp->fptr & (S_SIZ - 1)) == 0) {        /* On the sector boundary */
      if (--fp->sect_clust) {                   /* Decrement left sector counter */
        sect = fp->curr_sect + 1;               /* Get current sector */
      } else {                                  /* On the cluster boundary, get next cluster */
        clust = (fp->fptr == 0) ?
          fp->org_clust : get_cluster(fs, fp->curr_clust);
        if (clust < 2 || clust >= fs->max_clust)
          goto fr_error;
        fp->curr_clust = clust;                 /* Current cluster */
        sect = clust2sect(fs, clust);           /* Get current sector */
        fp->sect_clust = fs->sects_clust;       /* Re-initialize the left sector counter */
      }
#if !_FS_READONLY
      if(!move_fp_window(fp,0)) goto fr_error;
      //if (FPBUF.dirty) {        /* Flush file I/O buffer if needed */
      //  if (disk_write(fs->drive, FPBUF.data, fp->curr_sect, 1) != RES_OK)
      //    goto fr_error;
      //  FPBUF.dirty=FALSE;
      //}
#endif
      //printf("2 Sect=%d, curr_sect=%d\n",sect,fp->curr_sect);
      fp->curr_sect = sect;           /* Update current sector */
      cc = btr / S_SIZ;               /* When left bytes >= S_SIZ, */
      if (cc) {                       /* Read maximum contiguous sectors directly */
        if (cc > fp->sect_clust) cc = fp->sect_clust;
        if (disk_read(fs->drive, rbuff, sect, (BYTE)cc) != RES_OK)
          goto fr_error;
        fp->sect_clust -= (BYTE)(cc - 1);
        fp->curr_sect += cc - 1;
        rcnt = cc * S_SIZ;
        continue;
      }
      //if (disk_read(fs->drive, FPBUF.data, sect, 1) != RES_OK)  /* Load the sector into file I/O buffer */
      //  goto fr_error;
    }
    if(btr) {  /* if we actually have bytes to read in singles, copy them in */
      rcnt = S_SIZ - ((WORD)fp->fptr & (S_SIZ - 1));         /* Copy fractional bytes from file I/O buffer */
      if (rcnt > btr) rcnt = btr;
      if(!move_fp_window(fp,fp->curr_sect)) goto fr_error;   /* are we there or not? */
      memcpy(rbuff, &FPBUF.data[fp->fptr & (S_SIZ - 1)], rcnt);
    }
  }

  return FR_OK;

fr_error: /* Abort this file due to an unrecoverable error */
  fp->flag |= FA__ERROR;
  return FR_RW_ERROR;
}




#if !_FS_READONLY
/*-----------------------------------------------------------------------*/
/* Write File                                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_write (
  FIL *fp,          /* Pointer to the file object */
  const void *buff, /* Pointer to the data to be written */
  UINT btw,         /* Number of bytes to write */
  UINT *bw          /* Pointer to number of bytes written */
)
{
  DWORD clust, sect;
  UINT wcnt, cc;
  const BYTE *wbuff = buff;
  FRESULT res;
  FATFS *fs = fp->fs;


  *bw = 0;
  res = validate(fs, fp->id);                     /* Check validity of the object */
  if (res != FR_OK) return res;
  if (fp->flag & FA__ERROR) return FR_RW_ERROR;   /* Check error flag */
  if (!(fp->flag & FA_WRITE)) return FR_DENIED;   /* Check access mode */
  if (fp->fsize + btw < fp->fsize) return FR_OK;  /* File size cannot reach 4GB */

  for ( ;  btw;                                   /* Repeat until all data transferred */
    wbuff += wcnt, fp->fptr += wcnt, *bw += wcnt, btw -= wcnt) {
    if ((fp->fptr & (S_SIZ - 1)) == 0) {          /* On the sector boundary */
      if (--fp->sect_clust) {                     /* Decrement left sector counter */
        sect = fp->curr_sect + 1;                 /* Get current sector */
      } else {                                    /* On the cluster boundary, get next cluster */
        if (fp->fptr == 0) {                      /* Is top of the file */
          clust = fp->org_clust;
          if (clust == 0)                         /* No cluster is created yet */
            fp->org_clust = clust = create_chain(fs, 0);    /* Create a new cluster chain */
        } else {                                  /* Middle or end of file */
          clust = create_chain(fs, fp->curr_clust);         /* Trace or streach cluster chain */
        }
        if (clust == 0) break;                    /* Disk full */
        if (clust == 1 || clust >= fs->max_clust) goto fw_error;
        fp->curr_clust = clust;                   /* Current cluster */
        sect = clust2sect(fs, clust);             /* Get current sector */
        fp->sect_clust = fs->sects_clust;         /* Re-initialize the left sector counter */
      }
      if(!move_fp_window(fp,0)) goto fw_error;
      //if (FPBUF.dirty) {        /* Flush file I/O buffer if needed */
      //  if (disk_write(fs->drive, FPBUF.data, fp->curr_sect, 1) != RES_OK)
      //    goto fw_error;
      //  FPBUF.dirty=FALSE;
      //}
      fp->curr_sect = sect;                       /* Update current sector */
      cc = btw / S_SIZ;                           /* When left bytes >= S_SIZ, */
      if (cc) {                                   /* Write maximum contiguous sectors directly */
        if (cc > fp->sect_clust) cc = fp->sect_clust;
        if (disk_write(fs->drive, wbuff, sect, (BYTE)cc) != RES_OK)
          goto fw_error;
        fp->sect_clust -= (BYTE)(cc - 1);
        fp->curr_sect += cc - 1;
        wcnt = cc * S_SIZ;
        continue;
      }
      //if (fp->fptr < fp->fsize &&       /* Fill sector buffer with file data if needed */
      //  disk_read(fs->drive, FPBUF.data, sect, 1) != RES_OK)
      //    goto fw_error;
    }
    if(btw) {
      wcnt = S_SIZ - ((WORD)fp->fptr & (S_SIZ - 1));  /* Copy fractional bytes to file I/O buffer */
      if (wcnt > btw) wcnt = btw;
      if (
#if _USE_1_BUF == 0
	  fp->fptr < fp->fsize &&       /             * Fill sector buffer with file data if needed */
#endif
	  !move_fp_window(fp,fp->curr_sect))
	goto fw_error;
      memcpy(&FPBUF.data[fp->fptr & (S_SIZ - 1)], wbuff, wcnt);
      FPBUF.dirty=TRUE;
    }
  }

  if (fp->fptr > fp->fsize) fp->fsize = fp->fptr; /* Update file size if needed */
  fp->flag |= FA__WRITTEN;                        /* Set file changed flag */
  return FR_OK;

fw_error: /* Abort this file due to an unrecoverable error */
  fp->flag |= FA__ERROR;
  return FR_RW_ERROR;
}




/*-----------------------------------------------------------------------*/
/* Synchronize the file object                                           */
/*-----------------------------------------------------------------------*/

FRESULT f_sync (
  FIL *fp   /* Pointer to the file object */
)
{
  DWORD tim;
  BYTE *dir;
  FRESULT res;
  FATFS *fs = fp->fs;


  res = validate(fs, fp->id);       /* Check validity of the object */
  if (res == FR_OK) {
    if (fp->flag & FA__WRITTEN) {   /* Has the file been written? */
      /* Write back data buffer if needed */
      if(!move_fp_window(fp,0)) return FR_RW_ERROR;
      //if (FPBUF.dirty) {
      //  if (disk_write(fs->drive, FPBUF.data, fp->curr_sect, 1) != RES_OK)
      //    return FR_RW_ERROR;
      //  FPBUF.dirty=FALSE;
      //}
      /* Update the directory entry */
      if (!move_fs_window(fs, fp->dir_sect))
        return FR_RW_ERROR;
      dir = fp->dir_ptr;
      dir[DIR_Attr] |= AM_ARC;                        /* Set archive bit */
      ST_DWORD(&dir[DIR_FileSize], fp->fsize);        /* Update file size */
      ST_WORD(&dir[DIR_FstClusLO], fp->org_clust);    /* Update start cluster */
      ST_WORD(&dir[DIR_FstClusHI], fp->org_clust >> 16);
      tim = get_fattime();                            /* Updated time */
      ST_DWORD(&dir[DIR_WrtTime], tim);
      fp->flag &= ~FA__WRITTEN;
      res = sync(fs);
    }
  }
  return res;
}

#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Close File                                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_close (
  FIL *fp   /* Pointer to the file object to be closed */
)
{
  FRESULT res;


#if !_FS_READONLY
  res = f_sync(fp);
#else
  res = validate(fp->fs, fp->id);
#endif
  if (res == FR_OK)
    fp->fs = NULL;
  return res;
}




#if _FS_MINIMIZE <= 2
/*-----------------------------------------------------------------------*/
/* Seek File R/W Pointer                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_lseek (
  FIL *fp,    /* Pointer to the file object */
  DWORD ofs   /* File pointer from top of file */
)
{
  DWORD clust, csize;
  BYTE csect;
  FRESULT res;
  FATFS *fs = fp->fs;


  res = validate(fs, fp->id);         /* Check validity of the object */
  if (res != FR_OK) return res;
  if (fp->flag & FA__ERROR) return FR_RW_ERROR;
#if !_FS_READONLY
  if (!move_fp_window(fp,0)) goto fk_error;
  //if (FPBUF.dirty) {      /* Write-back dirty buffer if needed */
  //  if (disk_write(fs->drive, FPBUF.data, fp->curr_sect, 1) != RES_OK)
  //    goto fk_error;
  //  FPBUF.dirty=FALSE;
  //}
  if (ofs > fp->fsize && !(fp->flag & FA_WRITE))
#else
  if (ofs > fp->fsize)
#endif
    ofs = fp->fsize;
  fp->fptr = 0; fp->sect_clust = 1;   /* Set file R/W pointer to top of the file */

  /* Move file R/W pointer if needed */
  if (ofs) {
    clust = fp->org_clust;    /* Get start cluster */
#if !_FS_READONLY
    if (!clust) {             /* If the file does not have a cluster chain, create new cluster chain */
      clust = create_chain(fs, 0);
      if (clust == 1) goto fk_error;
      fp->org_clust = clust;
    }
#endif
    if (clust) {              /* If the file has a cluster chain, it can be followed */
      csize = (DWORD)fs->sects_clust * S_SIZ;     /* Cluster size in unit of byte */
      for (;;) {                                  /* Loop to skip leading clusters */
        fp->curr_clust = clust;                   /* Update current cluster */
        if (ofs <= csize) break;
#if !_FS_READONLY
        if (fp->flag & FA_WRITE)                  /* Check if in write mode or not */
          clust = create_chain(fs, clust);        /* Force streached if in write mode */
        else
#endif
          clust = get_cluster(fs, clust);         /* Only follow cluster chain if not in write mode */
        if (clust == 0) {                         /* Stop if could not follow the cluster chain */
          ofs = csize; break;
        }
        if (clust == 1 || clust >= fs->max_clust) goto fk_error;
        fp->fptr += csize;                        /* Update R/W pointer */
        ofs -= csize;
      }
      csect = (BYTE)((ofs - 1) / S_SIZ);          /* Sector offset in the cluster */
      fp->curr_sect = clust2sect(fs, clust) + csect;  /* Current sector */
      /* we can delay this... */
      //if ((ofs & (S_SIZ - 1)) &&          /* Load current sector if needed */
      //  disk_read(fs->drive, FPBUF.data, fp->curr_sect, 1) != RES_OK)
      //  goto fk_error;
      fp->sect_clust = fs->sects_clust - csect;   /* Left sector counter in the cluster */
      fp->fptr += ofs;                            /* Update file R/W pointer */
    }
  }
#if !_FS_READONLY
  if ((fp->flag & FA_WRITE) && fp->fptr > fp->fsize) {  /* Set updated flag if in write mode */
    fp->fsize = fp->fptr;
    fp->flag |= FA__WRITTEN;
  }
#endif

  return FR_OK;

fk_error: /* Abort this file due to an unrecoverable error */
  fp->flag |= FA__ERROR;
  return FR_RW_ERROR;
}




#if _FS_MINIMIZE <= 1
/*-----------------------------------------------------------------------*/
/* Create a directroy object                                             */
/*-----------------------------------------------------------------------*/

FRESULT f_opendir (
  DIR *dirobj,      /* Pointer to directory object to create */
  const char *path  /* Pointer to the directory path */
)
{
  BYTE *dir;
  char fn[8+3+1];
  FRESULT res;
  FATFS *fs;
#if _USE_LFN != 0
  DIR fileobj;
  const char* spath;
  UINT len;
#endif


  res = auto_mount(&path, &fs, 0);
  if (res != FR_OK) return res;
  dirobj->fs = fs;

#if _USE_LFN != 0
  res = trace_path(dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
  res = trace_path(dirobj, fn, path, &dir);   /* trace the file path */
#endif
  if (res == FR_OK) {                        /* Trace completed */
    if (dir != NULL) {                       /* It is not the root dir */
      if (dir[DIR_Attr] & AM_DIR) {          /* The entry is a directory */
        dirobj->clust = ((DWORD)LD_WORD(&dir[DIR_FstClusHI]) << 16) | LD_WORD(&dir[DIR_FstClusLO]);
        dirobj->sect = clust2sect(fs, dirobj->clust);
        dirobj->index = 2;
      } else {                               /* The entry is not a directory */
        res = FR_NO_FILE;
      }
    }
    dirobj->id = fs->id;
  }
  return res;
}




/*-----------------------------------------------------------------------*/
/* Read Directory Entry in Sequense                                      */
/*-----------------------------------------------------------------------*/


FRESULT f_readdir (
  DIR *dirobj,       /* Pointer to the directory object */
  FILINFO *finfo     /* Pointer to file information to return */
)
{
  BYTE *dir, c, res;
  FATFS *fs = dirobj->fs;
#if _USE_LFN != 0
  BYTE i,pos;
# ifdef _MAX_LFN_LENGTH
  BOOL skiplfn = FALSE;
# endif
  
  finfo->lfn_len=0;  /* set length to 0 */
  if (finfo->lfn) {
	finfo->lfn[0]=0;   /* set first char to null */
# if _USE_LFN_DBCS != 0 
	finfo->lfn[1]=0;
# endif
  }
#endif

  res = validate(fs, dirobj->id);     /* Check validity of the object */
  if (res != FR_OK) return res;

  finfo->fname[0] = 0;
  while (dirobj->sect) {
    if (!move_fs_window(fs, dirobj->sect))
      return FR_RW_ERROR;
    dir = &FSBUF.data[(dirobj->index & ((S_SIZ - 1) >> 5)) * 32]; /* pointer to the directory entry */
    c = *dir;
    if (c == 0) break;                /* Has it reached to end of dir? */
#if _USE_LFN != 0
    if (c != 0xE5) {                  /* Is it a valid entry? */
      if(finfo->lfn && ((dir[DIR_Attr] & AM_LFN) == AM_LFN)) {
        pos=((*dir & 0x1f)-1)*S_LFN_OFFSET;  /* get offset */
# ifdef _MAX_LFN_LENGTH
		if (skiplfn || pos >= _MAX_LFN_LENGTH) {
		  skiplfn = TRUE;
		  goto skippedlfn;
		}
# endif
        i=0;
        while(i<13) { 
          if(!dir[pgm_read_byte(LFN_pos+i)] && !dir[pgm_read_byte(LFN_pos+i)+1])
            break;
		  if (pos >= _MAX_LFN_LENGTH) {
			finfo->lfn_len = 0;
			i = 0;
			skiplfn = TRUE;
			break;
		  }
          finfo->lfn[pos]=dir[pgm_read_byte(LFN_pos+i)];
# if _USE_LFN_DBCS != 0 
          finfo->lfn[pos+1]=dir[pgm_read_byte(LFN_pos+i)+1];
# endif
          pos+=S_LFN_INCREMENT;
          i++;
        }
        finfo->lfn_len+=i;
      } else {
# ifdef _MAX_LFN_LENGTH
		skiplfn = FALSE;
# endif
		if (finfo->lfn) {
		  finfo->lfn[finfo->lfn_len*S_LFN_INCREMENT]=0;
# if _USE_LFN_DBCS != 0
		  finfo->lfn[finfo->lfn_len*S_LFN_INCREMENT+1]=0;
# endif
		}
		get_fileinfo(finfo, dir);
      }
    }
# ifdef _MAX_LFN_LENGTH
  skippedlfn:
#endif
#else
    if (c != 0xE5 && ((dir[DIR_Attr] & AM_LFN) != AM_LFN))        /* Is it a valid entry? */
      get_fileinfo(finfo, dir);
#endif
    if (!next_dir_entry(dirobj)) dirobj->sect = 0;                /* Next entry */
    if (finfo->fname[0]) break;       /* Found valid entry */
  }

  return FR_OK;
}




#if _FS_MINIMIZE == 0
/*-----------------------------------------------------------------------*/
/* Get File Status                                                       */
/*-----------------------------------------------------------------------*/

FRESULT f_stat (
  const char *path, /* Pointer to the file path */
  FILINFO *finfo    /* Pointer to file information to return */
)
{
  BYTE *dir;
  char fn[8+3+1];
  FRESULT res;
  DIR dirobj;
  FATFS *fs;
#if _USE_LFN != 0
  DIR fileobj;
  const char* spath;
  UINT len;
#endif


  res = auto_mount(&path, &fs, 0);
  if (res != FR_OK) return res;
  dirobj.fs = fs;

#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
  res = trace_path(&dirobj, fn, path, &dir);   /* trace the file path */
#endif
  if (res == FR_OK) {                          /* Trace completed */
    if (dir)                                   /* Found an object */
      get_fileinfo(finfo, dir);
    else                                       /* It is root dir */
      res = FR_INVALID_NAME;
  }

  return res;
}



#if !_FS_READONLY
/*-----------------------------------------------------------------------*/
/* Get Number of Free Clusters                                           */
/*-----------------------------------------------------------------------*/

FRESULT f_getfree (
  const char *drv,  /* Logical drive number */
  DWORD *nclust,    /* Pointer to the double word to return number of free clusters */
  FATFS **fatfs     /* Pointer to pointer to the file system object to return */
)
{
  DWORD n, clust, sect;
  BYTE fat, f, *p;
  FRESULT res;
  FATFS *fs;


  /* Get drive number */
  res = auto_mount(&drv, &fs, 0);
  if (res != FR_OK) return res;
  *fatfs = fs;

  /* If number of free cluster is valid, return it without cluster scan. */
  if (fs->free_clust <= fs->max_clust - 2) {
    *nclust = fs->free_clust;
    return FR_OK;
  }

  /* Count number of free clusters */
  fat = fs->fs_type;
  n = 0;
  if (fat == FS_FAT12) {
    clust = 2;
    do {
      if ((WORD)get_cluster(fs, clust) == 0) n++;
    } while (++clust < fs->max_clust);
  } else {
    clust = fs->max_clust;
    sect = fs->fatbase;
    f = 0; p = 0;
    do {
      if (!f) {
        if (!move_fs_window(fs, sect++)) return FR_RW_ERROR;
        p = FSBUF.data;
      }
      if (fat == FS_FAT16) {
        if (LD_WORD(p) == 0) n++;
        p += 2; f += 1;
      } else {
        if (LD_DWORD(p) == 0) n++;
        p += 4; f += 2;
      }
    } while (--clust);
  }
  fs->free_clust = n;
#if _USE_FSINFO
  if (fat == FS_FAT32) fs->fsi_flag = 1;
#endif

  *nclust = n;
  return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Delete a File or a Directory                                          */
/*-----------------------------------------------------------------------*/

FRESULT f_unlink (
  const char *path      /* Pointer to the file or directory path */
)
{
  BYTE *dir, *sdir;
  DWORD dclust, dsect;
  char fn[8+3+1];
  FRESULT res;
  DIR dirobj;
  FATFS *fs;
#if _USE_LFN != 0
  DIR fileobj;
  const char* spath;
  UINT len;
#endif


  res = auto_mount(&path, &fs, 1);
  if (res != FR_OK) return res;
  dirobj.fs = fs;

#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
  res = trace_path(&dirobj, fn, path, &dir);    /* trace the file path */
#endif
  if (res != FR_OK) return res;                 /* Trace failed */
  if (dir == NULL) return FR_INVALID_NAME;      /* It is the root directory */
  if (dir[DIR_Attr] & AM_RDO) return FR_DENIED; /* It is a R/O object */
  dsect = FSBUF.sect;
  dclust = ((DWORD)LD_WORD(&dir[DIR_FstClusHI]) << 16) | LD_WORD(&dir[DIR_FstClusLO]);

  if (dir[DIR_Attr] & AM_DIR) {                 /* It is a sub-directory */
#if _USE_CHDIR != 0
    if (dclust == fs->curr_dir)                 /* Never delete the current directory */
      return FR_DIR_NOT_EMPTY;
#endif
    dirobj.clust = dclust;                      /* Check if the sub-dir is empty or not */
    dirobj.sect = clust2sect(fs, dclust);
    dirobj.index = 2;
    do {
      if (!move_fs_window(fs, dirobj.sect)) return FR_RW_ERROR;
      sdir = &FSBUF.data[(dirobj.index & ((S_SIZ - 1) >> 5)) * 32];
      if (sdir[DIR_Name] == 0) break;
      if (sdir[DIR_Name] != 0xE5 && !(sdir[DIR_Attr] & AM_VOL))
        return FR_DIR_NOT_EMPTY;                /* The directory is not empty */
    } while (next_dir_entry(&dirobj));
  }

#if _USE_LFN != 0
  len=(len+25)/13;
  while(len--) {
    if (!move_fs_window(fs, fileobj.sect)) return FR_RW_ERROR;  /* Mark the directory entry 'deleted' */
    dir = &FSBUF.data[(fileobj.index & ((S_SIZ - 1) >> 5)) * 32];
    dir[DIR_Name] = 0xE5;
    FSBUF.dirty = TRUE;
    if (len && !next_dir_entry(&fileobj))             /* Next directory pointer */
      return FR_RW_ERROR;
  }    
#else
  if (!move_fs_window(fs, dsect)) return FR_RW_ERROR; /* Mark the directory entry 'deleted' */
  dir[DIR_Name] = 0xE5;
  FSBUF.dirty = TRUE;
#endif
  if (!remove_chain(fs, dclust)) return FR_RW_ERROR;  /* Remove the cluster chain */

  return sync(fs);
}




/*-----------------------------------------------------------------------*/
/* Create a Directory                                                    */
/*-----------------------------------------------------------------------*/

FRESULT f_mkdir (
  const char *path    /* Pointer to the directory path */
)
{
  BYTE *dir, *fw, n;
  char fn[8+3+1];
  DWORD sect, dsect, dclust, pclust, tim;
  FRESULT res;
  DIR dirobj;
  FATFS *fs;
#if _USE_LFN != 0
  UINT len;
  DIR fileobj;
  const char* spath;
#endif


  res = auto_mount(&path, &fs, 1);
  if (res != FR_OK) return res;
  dirobj.fs = fs;

#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
  res = trace_path(&dirobj, fn, path, &dir);   /* trace the file path */
#endif
  if (res == FR_OK) return FR_EXIST;           /* Any file or directory is already existing */
  if (res != FR_NO_FILE) return res;

#if _USE_CHDIR != 0
# if _USE_LFN != 0
  if((len==1 && spath[0]=='.') || (len==2 && spath[0]=='.' && spath[1]=='.'))
    return FR_INVALID_NAME;
  else
# endif
  if(fn[0]=='.')
    return FR_INVALID_NAME;
#endif
#if _USE_LFN != 0
  res = reserve_direntry(&dirobj, &dir,len);   /* Reserve a directory entry */
#else
  res = reserve_direntry(&dirobj, &dir);       /* Reserve a directory entry */
#endif
  if (res != FR_OK) return res;
  sect = FSBUF.sect;
  dclust = create_chain(fs, 0);                /* Allocate a cluster for new directory table */
  if (dclust == 1) return FR_RW_ERROR;
  dsect = clust2sect(fs, dclust);
  if (!dsect) return FR_DENIED;
  if (!move_fs_window(fs, dsect)) return FR_RW_ERROR;

  fw = FSBUF.data;
  memset(fw, 0, S_SIZ);                        /* Clear the new directory table */
  for (n = 1; n < fs->sects_clust; n++) {
    if (disk_write(fs->drive, fw, ++dsect, 1) != RES_OK)
      return FR_RW_ERROR;
  }
  memset(&fw[DIR_Name], ' ', 8+3);             /* Create "." entry */
  fw[DIR_Name] = '.';
  fw[DIR_Attr] = AM_DIR;
  tim = get_fattime();
  ST_DWORD(&fw[DIR_WrtTime], tim);
  memcpy(&fw[32], &fw[0], 32); fw[33] = '.';   /* Create ".." entry */
  ST_WORD(&fw[   DIR_FstClusLO], dclust);
  ST_WORD(&fw[   DIR_FstClusHI], dclust >> 16);
  pclust = dirobj.sclust;
  if (fs->fs_type == FS_FAT32 && pclust == fs->dirbase) pclust = 0;
  ST_WORD(&fw[32+DIR_FstClusLO], pclust);
  ST_WORD(&fw[32+DIR_FstClusHI], pclust >> 16);
  FSBUF.dirty = TRUE;

  if (!move_fs_window(fs, sect)) return FR_RW_ERROR;
#if _USE_LFN != 0
  if(len && add_direntry(&dirobj,&dir,spath,len,fn)) return FR_RW_ERROR;
#endif
  memset(&dir[0], 0, 32);                      /* Initialize the new entry */
  memcpy(&dir[DIR_Name], fn, 8+3);             /* Name */
  dir[DIR_NTres] = fn[11];
  FSBUF.dirty = TRUE;
  dir[DIR_Attr] = AM_DIR;                      /* Attribute */
  ST_DWORD(&dir[DIR_WrtTime], tim);            /* Crated time */
  ST_WORD(&dir[DIR_FstClusLO], dclust);        /* Table start cluster */
  ST_WORD(&dir[DIR_FstClusHI], dclust >> 16);

  return sync(fs);
}



#if _USE_CHDIR != 0
/*-----------------------------------------------------------------------*/
/* Change the current directory                                          */
/*-----------------------------------------------------------------------*/

FRESULT f_chdir (
  const char *path  /* Pointer to the file name */
)
{
  FRESULT res;
  BYTE *dir;
  DIR dirobj;
  char fn[8+3+1];
  FATFS *fs;
#if _USE_LFN != 0
  UINT len;
  DIR fileobj;
  const char* spath;
#endif

  res = auto_mount(&path, &fs, 0);
  if (res != FR_OK) return res;
  dirobj.fs = fs;

  /* Trace the file path */
#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
  res = trace_path(&dirobj, fn, path, &dir);   /* trace the file path */
#endif

  if (res == FR_OK) {
    if (dir == NULL) {
      fs->curr_dir = 0;
    } else if (dir[DIR_Attr] & AM_DIR) {
      fs->curr_dir =
        ((DWORD)LD_WORD(&dir[DIR_FstClusHI]) << 16) |
        LD_WORD(&dir[DIR_FstClusLO]);
      } else
        return FR_NOT_DIRECTORY;
  }

  return res;
}
#endif




/*-----------------------------------------------------------------------*/
/* Change File Attribute                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_chmod (
  const char *path, /* Pointer to the file path */
  BYTE value,       /* Attribute bits */
  BYTE mask         /* Attribute mask to change */
)
{
  FRESULT res;
  BYTE *dir;
  DIR dirobj;
  char fn[8+3+1];
  FATFS *fs;
#if _USE_LFN != 0
  UINT len;
  DIR fileobj;
  const char* spath;
#endif


  res = auto_mount(&path, &fs, 1);
  if (res == FR_OK) {
    dirobj.fs = fs;
#if _USE_LFN != 0
    res = trace_path(&dirobj, fn, path, &dir, &fileobj, &spath, &len);
#else
    res = trace_path(&dirobj, fn, path, &dir);   /* trace the file path */
#endif
    if (res == FR_OK) {                          /* Trace completed */
      if (dir == NULL) {
        res = FR_INVALID_NAME;
      } else {
        mask &= AM_RDO|AM_HID|AM_SYS|AM_ARC;     /* Valid attribute mask */
        dir[DIR_Attr] = (value & mask) | (dir[DIR_Attr] & (BYTE)~mask); /* Apply attribute change */
        res = sync(fs);
      }
    }
  }
  return res;
}




/*-----------------------------------------------------------------------*/
/* Rename File/Directory                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_rename (
  const char *path_old, /* Pointer to the old name */
  const char *path_new  /* Pointer to the new name */
)
{
  FRESULT res;
  DWORD sect_old;
  BYTE *dir_old, *dir_new, direntry[32-11];
  DIR dirobj;
  char fn[8+3+1];
  FATFS *fs;
#if _USE_LFN != 0
  UINT len_old, len_new;
  DIR fileobj;
  const char* spath;
#endif


  res = auto_mount(&path_old, &fs, 1);
  if (res != FR_OK) return res;
  dirobj.fs = fs;

#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path_old, &dir_old, &fileobj, &spath, &len_old);
#else
  res = trace_path(&dirobj, fn, path_old, &dir_old);   /* trace the file path */
#endif
  if (res != FR_OK) return res;                        /* The old object is not found */
  if (!dir_old) return FR_NO_FILE;
  sect_old = FSBUF.sect;                               /* Save the object information */
  memcpy(direntry, &dir_old[DIR_Attr], 32-11);

#if _USE_LFN != 0
  res = trace_path(&dirobj, fn, path_new, &dir_new, &fileobj, &spath, &len_new);
#else
  res = trace_path(&dirobj, fn, path_new, &dir_new);   /* trace the file path */
#endif
  if (res == FR_OK) return FR_EXIST;                   /* The new object name is already existing */
  if (res != FR_NO_FILE) return res;                   /* Is there no old name? */
#if _USE_CHDIR != 0
# if _USE_LFN != 0
  if((len_new==1 && spath[0]=='.') || (len_new==2 && spath[0]=='.' && spath[1]=='.'))
    return FR_INVALID_NAME;
  else
# endif
  if(fn[0]=='.')
    return FR_INVALID_NAME;
#endif
#if _USE_LFN != 0
  res = reserve_direntry(&dirobj, &dir_new,len_new);   /* Reserve a directory entry */
  if (res != FR_OK) return res;

  if(len_new && add_direntry(&dirobj,&dir_new,spath,len_new,fn))  /* need to get shortname, check it, and add all names */
    return FR_RW_ERROR;
#else
  res = reserve_direntry(&dirobj, &dir_new);           /* Reserve a directory entry */
  if (res != FR_OK) return res;
#endif
  memcpy(&dir_new[DIR_Attr], direntry, 32-11);         /* Create new entry */
  memcpy(&dir_new[DIR_Name], fn, 8+3);
  dir_new[DIR_NTres] = fn[11];
  FSBUF.dirty = TRUE;

#if _USE_LFN != 0
  /* Trace it again, fileobj was clobbered while tracing the new path */
  res = trace_path(&dirobj, fn, path_old, &dir_old, &fileobj, &spath, &len_old);
  len_old=(len_old+25)/13;
  while(len_old--) {
    if (!move_fs_window(fs, fileobj.sect)) return FR_RW_ERROR;    /* Mark the directory entry 'deleted' */
    dir_old = &FSBUF.data[(fileobj.index & ((S_SIZ - 1) >> 5)) * 32];
    dir_old[DIR_Name] = 0xE5;
    FSBUF.dirty = TRUE;
    if (!next_dir_entry(&fileobj))                     /* Next directory pointer */
      return FR_RW_ERROR;
  }    
#else
  if (!move_fs_window(fs, sect_old)) return FR_RW_ERROR;          /* Remove old entry */
  dir_old[DIR_Name] = 0xE5;
#endif

  return sync(fs);
}



#if _USE_MKFS
/*-----------------------------------------------------------------------*/
/* Create File System on the Drive                                       */
/*-----------------------------------------------------------------------*/

#define N_ROOTDIR 512           /* Multiple of 32 and <= 2048 */
#define N_FATS    1             /* 1 or 2 */
#define MAX_SECTOR  64000000UL  /* Maximum partition size */
#define MIN_SECTOR  2000UL      /* Minimum partition size */


FRESULT f_mkfs (
  BYTE drv,         /* Logical drive number */
  BYTE partition,   /* Partitioning rule 0:FDISK, 1:SFD */
  WORD allocsize    /* Allocation unit size [bytes] */
)
{
  BYTE fmt, m, *tbl;
  DWORD b_part, b_fat, b_dir, b_data;     /* Area offset (LBA) */
  DWORD n_part, n_rsv, n_fat, n_dir;      /* Area size */
  DWORD n_clust, n;
  FATFS *fs;
  DSTATUS stat;


  /* Check validity of the parameters */
  if (drv >= _DRIVES) return FR_INVALID_DRIVE;
  if (partition >= 2) return FR_MKFS_ABORTED;
  for (n = 512; n <= 32768U && n != allocsize; n <<= 1);
  if (n != allocsize) return FR_MKFS_ABORTED;

  /* Check mounted drive and clear work area */
  fs = FatFs[drv];
  if (!fs) return FR_NOT_ENABLED;
  memset(fs, 0, sizeof(FATFS));
  drv = LD2PD(drv);

  /* Get disk statics */
  stat = disk_initialize(drv);
  if (stat & STA_NOINIT) return FR_NOT_READY;
  if (stat & STA_PROTECT) return FR_WRITE_PROTECTED;
  if (disk_ioctl(drv, GET_SECTOR_COUNT, &n_part) != RES_OK || n_part < MIN_SECTOR)
    return FR_MKFS_ABORTED;
  if (n_part > MAX_SECTOR) n_part = MAX_SECTOR;
  b_part = (!partition) ? 63 : 0;         /* Boot sector */
  n_part -= b_part;
#if S_MAX_SIZ > 512                       /* Check disk sector size */
  if (disk_ioctl(drv, GET_SECTOR_SIZE, &S_SIZ) != RES_OK
    || S_SIZ > S_MAX_SIZ
    || S_SIZ > allocsize)
    return FR_MKFS_ABORTED;
#endif
  allocsize /= S_SIZ;                     /* Number of sectors per cluster */

  /* Pre-compute number of clusters and FAT type */
  n_clust = n_part / allocsize;
  fmt = FS_FAT12;
  if (n_clust >= 0xFF7) fmt = FS_FAT16;
  if (n_clust >= 0xFFF7) fmt = FS_FAT32;

  /* Determine offset and size of FAT structure */
  switch (fmt) {
  case FS_FAT12:
    n_fat = ((n_clust * 3 + 1) / 2 + 3 + S_SIZ - 1) / S_SIZ;
    n_rsv = 1 + partition;
    n_dir = N_ROOTDIR * 32 / S_SIZ;
    break;
  case FS_FAT16:
    n_fat = ((n_clust * 2) + 4 + S_SIZ - 1) / S_SIZ;
    n_rsv = 1 + partition;
    n_dir = N_ROOTDIR * 32 / S_SIZ;
    break;
  default:
    n_fat = ((n_clust * 4) + 8 + S_SIZ - 1) / S_SIZ;
    n_rsv = 33 - partition;
    n_dir = 0;
  }
  b_fat = b_part + n_rsv;                 /* FATs start sector */
  b_dir = b_fat + n_fat * N_FATS;         /* Directory start sector */
  b_data = b_dir + n_dir;                 /* Data start sector */

  /* Align data start sector to erase block boundary (for flash memory media) */
  if (disk_ioctl(drv, GET_BLOCK_SIZE, &n) != RES_OK) return FR_MKFS_ABORTED;
  n = (b_data + n - 1) & ~(n - 1);
  n_fat += (n - b_data) / N_FATS;
  /* b_dir and b_data are no longer used below */

  /* Determine number of cluster and final check of validity of the FAT type */
  n_clust = (n_part - n_rsv - n_fat * N_FATS - n_dir) / allocsize;
  if (   (fmt == FS_FAT16 && n_clust < 0xFF7)
    || (fmt == FS_FAT32 && n_clust < 0xFFF7))
    return FR_MKFS_ABORTED;

  /* Create partition table if needed */
  if (!partition) {
    DWORD n_disk = b_part + n_part;

    tbl = &FSBUF.data[MBR_Table];
    ST_DWORD(&tbl[0], 0x00010180);        /* Partition start in CHS */
    if (n_disk < 63UL * 255 * 1024) {     /* Partition end in CHS */
      n_disk = n_disk / 63 / 255;
      tbl[7] = (BYTE)n_disk;
      tbl[6] = (BYTE)((n_disk >> 2) | 63);
    } else {
      ST_WORD(&tbl[6], 0xFFFF);
    }
    tbl[5] = 254;
    if (fmt != FS_FAT32)                  /* System ID */
      tbl[4] = (n_part < 0x10000) ? 0x04 : 0x06;
    else
      tbl[4] = 0x0c;
    ST_DWORD(&tbl[8], 63);                /* Partition start in LBA */
    ST_DWORD(&tbl[12], n_part);           /* Partition size in LBA */
    ST_WORD(&tbl[64], 0xAA55);            /* Signature */
    if (disk_write(drv, FSBUF.data, 0, 1) != RES_OK)
      return FR_RW_ERROR;
  }

  /* Create boot record */
  tbl = FSBUF.data;                       /* Clear buffer */
  memset(tbl, 0, S_SIZ);
  ST_DWORD(&tbl[BS_jmpBoot], 0x90FEEB);   /* Boot code (jmp $, nop) */
  ST_WORD(&tbl[BPB_BytsPerSec], S_SIZ);   /* Sector size */
  tbl[BPB_SecPerClus] = (BYTE)allocsize;  /* Sectors per cluster */
  ST_WORD(&tbl[BPB_RsvdSecCnt], n_rsv);   /* Reserved sectors */
  tbl[BPB_NumFATs] = N_FATS;              /* Number of FATs */
  ST_WORD(&tbl[BPB_RootEntCnt], S_SIZ / 32 * n_dir);      /* Number of rootdir entries */
  if (n_part < 0x10000) {                 /* Number of total sectors */
    ST_WORD(&tbl[BPB_TotSec16], n_part);
  } else {
    ST_DWORD(&tbl[BPB_TotSec32], n_part);
  }
  tbl[BPB_Media] = 0xF8;                  /* Media descripter */
  ST_WORD(&tbl[BPB_SecPerTrk], 63);       /* Number of sectors per track */
  ST_WORD(&tbl[BPB_NumHeads], 255);       /* Number of heads */
  ST_DWORD(&tbl[BPB_HiddSec], b_part);    /* Hidden sectors */
  if (fmt != FS_FAT32) {
    ST_WORD(&tbl[BPB_FATSz16], n_fat);    /* Number of secters per FAT */
    tbl[BS_DrvNum] = 0x80;                /* Drive number */
    tbl[BS_BootSig] = 0x29;               /* Extended boot signature */
    memcpy(&tbl[BS_VolLab], "NO NAME    FAT     ", 19);   /* Volume lavel, FAT signature */
  } else {
    ST_DWORD(&tbl[BPB_FATSz32], n_fat);   /* Number of secters per FAT */
    ST_DWORD(&tbl[BPB_RootClus], 2);      /* Root directory cluster (2) */
    ST_WORD(&tbl[BPB_FSInfo], 1);         /* FSInfo record (bs+1) */
    ST_WORD(&tbl[BPB_BkBootSec], 6);      /* Backup boot record (bs+6) */
    tbl[BS_DrvNum32] = 0x80;              /* Drive number */
    tbl[BS_BootSig32] = 0x29;             /* Extended boot signature */
    memcpy(&tbl[BS_VolLab32], "NO NAME    FAT32   ", 19); /* Volume lavel, FAT signature */
  }
  ST_WORD(&tbl[BS_55AA], 0xAA55);         /* Signature */
  if (disk_write(drv, tbl, b_part+0, 1) != RES_OK)
    return FR_RW_ERROR;
  if (fmt == FS_FAT32)
    disk_write(drv, tbl, b_part+6, 1);

  /* Initialize FAT area */
  for (m = 0; m < N_FATS; m++) {
    memset(tbl, 0, S_SIZ);                /* 1st sector of the FAT  */
    if (fmt != FS_FAT32) {
      n = (fmt == FS_FAT12) ? 0x00FFFFF8 : 0xFFFFFFF8;
      ST_DWORD(&tbl[0], n);               /* Reserve cluster #0-1 (FAT12/16) */
    } else {
      ST_DWORD(&tbl[0], 0xFFFFFFF8);      /* Reserve cluster #0-1 (FAT32) */
      ST_DWORD(&tbl[4], 0xFFFFFFFF);
      ST_DWORD(&tbl[8], 0x0FFFFFFF);      /* Reserve cluster #2 for root dir */
    }
    if (disk_write(drv, tbl, b_fat++, 1) != RES_OK)
      return FR_RW_ERROR;
    memset(tbl, 0, S_SIZ);                /* Following FAT entries are filled by zero */
    for (n = 1; n < n_fat; n++) {
      if (disk_write(drv, tbl, b_fat++, 1) != RES_OK)
        return FR_RW_ERROR;
    }
  }

  /* Initialize Root directory */
  m = (BYTE)((fmt == FS_FAT32) ? allocsize : n_dir);
  do {
    if (disk_write(drv, tbl, b_fat++, 1) != RES_OK)
      return FR_RW_ERROR;
  } while (--m);

  /* Create FSInfo record if needed */
  if (fmt == FS_FAT32) {
    ST_WORD(&tbl[BS_55AA], 0xAA55);
    ST_DWORD(&tbl[FSI_LeadSig], 0x41615252);
    ST_DWORD(&tbl[FSI_StrucSig], 0x61417272);
    ST_DWORD(&tbl[FSI_Free_Count], n_clust - 1);
    ST_DWORD(&tbl[FSI_Nxt_Free], 0xFFFFFFFF);
    disk_write(drv, tbl, b_part+1, 1);
    disk_write(drv, tbl, b_part+7, 1);
  }

  return (disk_ioctl(drv, CTRL_SYNC, NULL) == RES_OK) ? FR_OK : FR_RW_ERROR;
}

#endif /* _USE_MKFS */
#endif /* !_FS_READONLY */
#endif /* _FS_MINIMIZE == 0 */
#endif /* _FS_MINIMIZE <= 1 */
#endif /* _FS_MINIMIZE <= 2 */

#if _USE_EXT != 0
#include "ff-ext.c"
#endif

/*
Emacs stuff:
  Local Variables:
  c-basic-offset: 2
  tab-width: 4
  indent-tab-mode: nil
  End:
*/