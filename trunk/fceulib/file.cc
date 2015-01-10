/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Xodnizel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fstream>

#ifndef WIN32
#include <zlib.h>
#endif

#include "types.h"
#include "file.h"
#include "utils/endian.h"
#include "utils/memory.h"
#include "utils/md5.h"
#include "driver.h"
#include "types.h"
#include "fceu.h"
#include "state.h"
#include "driver.h"
#include "utils/xstring.h"

using namespace std;

// Probably should be false since there are no movies any more -tom7
static constexpr bool bindSavestate = true;	//Toggle that determines if a savestate filename will include the movie filename
static string BaseDirectory;
static char FileBase[2048];
static char FileBaseDirectory[2048];


// XXX I think these can go --tom7
namespace {
struct FCEUARCHIVEFILEINFO_ITEM {
  std::string name;
  uint32 size, index;
};

class FCEUARCHIVEFILEINFO : public std::vector<FCEUARCHIVEFILEINFO_ITEM> {
public:
  void FilterByExtension(const char** ext);
};

struct ArchiveScanRecord {
  ArchiveScanRecord() {}
  ArchiveScanRecord(int _type, int _numFiles) {
    type = _type;
    numFilesInArchive = _numFiles;
  }
  int type = -1;

  //be careful: this is the number of files in the archive.
  //the size of the files variable might be different.
  int numFilesInArchive = 0;

  FCEUARCHIVEFILEINFO files;

  bool isArchive() { return type != -1; }
};
}


static uint64 FCEU_ftell(FceuFile *fp) {
  return fp->stream->ftell();
}

namespace {
struct FileBaseInfo {
  std::string filebase, filebasedirectory, ext;
  FileBaseInfo() {}
  FileBaseInfo(const std::string &fbd, 
	       const std::string &fb, 
	       const std::string &ex) {
    filebasedirectory = fbd;
    filebase = fb;
    ext = ex;
  }
};
}

// Can probably go. -tom7
static void FCEU_SplitArchiveFilename(string src, string& archive, 
				      string& file, string& fileToOpen) {
  size_t pipe = src.find_first_of('|');
  if (pipe == string::npos) {
    archive = "";
    file = src;
    fileToOpen = src;
  } else {
    archive = src.substr(0,pipe);
    file = src.substr(pipe+1);
    fileToOpen = archive;
  }
}

FileBaseInfo DetermineFileBase(const char *f) {
  char drv[PATH_MAX], dir[PATH_MAX], name[PATH_MAX], ext[PATH_MAX];
  splitpath(f,drv,dir,name,ext);

  if (dir[0] == 0) strcpy(dir,".");

  return FileBaseInfo((string)drv + dir,name,ext);
}

inline FileBaseInfo DetermineFileBase(const string& str) { 
  return DetermineFileBase(str.c_str());
}


// I think this means that all archives appear empty, and this can be
// simplified away?
static ArchiveScanRecord FCEUD_ScanArchive(const std::string &fname) {
  return ArchiveScanRecord(); 
}


FceuFile *FCEU_fopen(const char *path, char *mode, char *ext, int index, 
		     const char **extensions) {
  FceuFile *fceufp=0;

  bool read = (string)mode == "rb";
  bool write = (string)mode == "wb";
  if ((read&&write) || (!read&&!write)) {
    FCEU_PrintError("invalid file open mode specified (only wb and rb are supported)");
    return 0;
  }

  string archive,fname,fileToOpen;
  FCEU_SplitArchiveFilename(path,archive,fname,fileToOpen);

  // This is weird -- the only supported mode is read, and write always fails?
  // -tom7
  if (!read) {
    return nullptr;
  }

  ArchiveScanRecord asr = FCEUD_ScanArchive(fileToOpen);
  asr.files.FilterByExtension(extensions);
  if (asr.isArchive()) {
    // Archive files are no longer supported. -tom7
    return nullptr;
  }

  //if the archive contained no files, try to open it the old fashioned way
  EMUFILE_FILE* fp = FCEUD_UTF8_fstream(fileToOpen,mode);
  if (!fp || (fp->get_fp() == NULL)) {
    return 0;
  }

  // Here we used to try zip files. -tom7

  // And here we used to try gzip. -tom7
  {
    uint32 magic;

    // XXX can just skip these gets right? fseek
    // is putting us back at the beginning?
    magic = fp->fgetc();
    magic|=fp->fgetc()<<8;
    magic|=fp->fgetc()<<16;
    fp->fseek(0,SEEK_SET);
  }

  // open a plain old file
  fceufp = new FceuFile();
  fceufp->filename = fileToOpen;
  fceufp->logicalPath = fileToOpen;
  fceufp->fullFilename = fileToOpen;
  fceufp->archiveIndex = -1;
  fceufp->stream = fp;
  FCEU_fseek(fceufp,0,SEEK_END);
  fceufp->size = FCEU_ftell(fceufp);
  FCEU_fseek(fceufp,0,SEEK_SET);

  return fceufp;
}

int FCEU_fclose(FceuFile *fp) {
  delete fp;
  return 1;
}

uint64 FCEU_fread(void *ptr, size_t size, size_t nmemb, FceuFile *fp) {
  return fp->stream->fread((char*)ptr,size*nmemb);
}

int FCEU_fseek(FceuFile *fp, long offset, int whence) {
  fp->stream->fseek(offset,whence);
  return FCEU_ftell(fp);
}

int FCEU_read32le(uint32 *Bufo, FceuFile *fp) {
  return read32le(Bufo, fp->stream);
}

int FCEU_fgetc(FceuFile *fp) {
  return fp->stream->fgetc();
}

uint64 FCEU_fgetsize(FceuFile *fp) {
  return fp->size;
}

// TODO(tom7): We should probably not let this thing save battery backup
// files; they should be serialized as part of the state at most.
string FCEU_MakeSaveFilename() {
  return (string)FileBase + ".sav";
}

string FCEU_MakeFDSFilename() {
  return (string)FileBase + ".fds";
}

string FCEU_MakeFDSROMFilename() {
  return "disksys.rom";
}

string FCEU_MakePaletteFilename() {
  return (string)FileBase + ".pal";
}

void GetFileBase(const char *f) {
  FileBaseInfo fbi = DetermineFileBase(f);
  strcpy(FileBase,fbi.filebase.c_str());
  strcpy(FileBaseDirectory,fbi.filebasedirectory.c_str());
}

void FCEUARCHIVEFILEINFO::FilterByExtension(const char** ext) {
  if (!ext) return;
  int count = size();
  for (int i=count-1;i>=0;i--) {
    string fext = getExtension((*this)[i].name.c_str());
    const char** currext = ext;
    while (*currext) {
      if (fext == *currext)
	goto ok;
      currext++;
    }
    this->erase(begin()+i);
  ok:;
  }
}
