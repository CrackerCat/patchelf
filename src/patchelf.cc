#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#include <elf.h>

using namespace std;


const unsigned int pageSize = 4096;


static bool debugMode = false;

static string fileName;


off_t fileSize, maxSize;
unsigned char * contents = 0;


class ElfFile {
    Elf32_Ehdr * hdr;
    vector<Elf32_Phdr> phdrs;
    vector<Elf32_Shdr> shdrs;

    bool changed;

    typedef string SectionName;
    typedef map<SectionName, string> ReplacedSections;

    ReplacedSections replacedSections;

    string sectionNames; /* content of the .shstrtab section */

public:

    ElfFile() 
    {
        changed = false;
    }

    bool isChanged()
    {
        return changed;
    }
    
    void parse();

    void shiftFile(unsigned int extraPages, Elf32_Addr startPage);

    string getSectionName(const Elf32_Shdr & shdr);

    Elf32_Shdr * findSection2(const SectionName & sectionName);

    Elf32_Shdr & findSection(const SectionName & sectionName);
    
    string & replaceSection(const SectionName & sectionName,
        unsigned int size);

    void rewriteSections();

    string getInterpreter();

    void setInterpreter(const string & newInterpreter);

    typedef enum { rpPrint, rpShrink, rpSet } RPathOp;

    void modifyRPath(RPathOp op, string newRPath);
};


static void debug(const char * format, ...)
{
    if (debugMode) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}


static void error(string msg)
{
    if (errno) perror(msg.c_str()); else fprintf(stderr, "%s\n", msg.c_str());
    exit(1);
}


static void growFile(off_t newSize)
{
    if (newSize > maxSize) error("maximum file size exceeded");
    if (newSize <= fileSize) return;
    if (newSize > fileSize)
        memset(contents + fileSize, 0, newSize - fileSize);
    fileSize = newSize;
}


static void readFile(string fileName, mode_t * fileMode)
{
    struct stat st;
    if (stat(fileName.c_str(), &st) != 0) error("stat");
    fileSize = st.st_size;
    *fileMode = st.st_mode;
    maxSize = fileSize + 4 * 1024 * 1024;
    
    contents = (unsigned char *) malloc(fileSize + maxSize);
    if (!contents) abort();

    int fd = open(fileName.c_str(), O_RDONLY);
    if (fd == -1) error("open");

    if (read(fd, contents, fileSize) != fileSize) error("read");
    
    close(fd);
}


static void checkPointer(void * p, unsigned int size)
{
    unsigned char * q = (unsigned char *) p;
    assert(q >= contents && q + size <= contents + fileSize);
}


void ElfFile::parse()
{
    /* Check the ELF header for basic validity. */
    if (fileSize < sizeof(Elf32_Ehdr)) error("missing ELF header");

    hdr = (Elf32_Ehdr *) contents;

    if (memcmp(hdr->e_ident, ELFMAG, 4) != 0)
        error("not an ELF executable");

    if (contents[EI_CLASS] != ELFCLASS32 ||
        contents[EI_DATA] != ELFDATA2LSB ||
        contents[EI_VERSION] != EV_CURRENT)
        error("ELF executable is not 32-bit, little-endian, version 1");

    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
        error("wrong ELF type");
    
    if (hdr->e_phoff + hdr->e_phnum * hdr->e_phentsize > fileSize)
        error("missing program headers");
    
    if (hdr->e_shoff + hdr->e_shnum * hdr->e_shentsize > fileSize)
        error("missing section headers");

    if (hdr->e_phentsize != sizeof(Elf32_Phdr))
        error("program headers have wrong size");

    /* Copy the program and section headers. */
    for (int i = 0; i < hdr->e_phnum; ++i)
        phdrs.push_back(* ((Elf32_Phdr *) (contents + hdr->e_phoff) + i));
    
    for (int i = 0; i < hdr->e_shnum; ++i)
        shdrs.push_back(* ((Elf32_Shdr *) (contents + hdr->e_shoff) + i));

    /* Get the section header string table section (".shstrtab").  Its
       index in the section header table is given by e_shstrndx field
       of the ELF header. */
    unsigned int shstrtabIndex = hdr->e_shstrndx;
    assert(shstrtabIndex < shdrs.size());
    unsigned int shstrtabSize = shdrs[shstrtabIndex].sh_size;
    char * shstrtab = (char * ) contents + shdrs[shstrtabIndex].sh_offset;
    checkPointer(shstrtab, shstrtabSize);

    assert(shstrtabSize > 0);
    assert(shstrtab[shstrtabSize - 1] == 0);

    sectionNames = string(shstrtab, shstrtabSize);
}


static void writeFile(string fileName, mode_t fileMode)
{
    string fileName2 = fileName + "_patchelf_tmp";
    
    int fd = open(fileName2.c_str(),
        O_CREAT | O_TRUNC | O_WRONLY, 0700);
    if (fd == -1) error("open");

    if (write(fd, contents, fileSize) != fileSize) error("write");
    
    if (close(fd) != 0) error("close");

    if (chmod(fileName2.c_str(), fileMode) != 0) error("chmod");

    if (rename(fileName2.c_str(), fileName.c_str()) != 0) error("rename");
}


static unsigned int roundUp(unsigned int n, unsigned int m)
{
    return ((n - 1) / m + 1) * m;
}


void ElfFile::shiftFile(unsigned int extraPages, Elf32_Addr startPage)
{
    /* Move the entire contents of the file `extraPages' pages
       further. */
    unsigned int oldSize = fileSize;
    unsigned int shift = extraPages * pageSize;
    growFile(fileSize + extraPages * pageSize);
    memmove(contents + extraPages * pageSize, contents, oldSize);
    memset(contents + sizeof(Elf32_Ehdr), 0, shift - sizeof(Elf32_Ehdr));

    /* Adjust the ELF header. */
    hdr->e_phoff = sizeof(Elf32_Ehdr);
    hdr->e_shoff += shift;
    
    /* Update the offsets in the section headers. */
    for (int i = 0; i < hdr->e_shnum; ++i)
        shdrs[i].sh_offset += shift;
    
    /* Update the offsets in the program headers. */
    for (int i = 0; i < hdr->e_phnum; ++i)
        phdrs[i].p_offset += shift;

    /* Add a segment that maps the new program/section headers and
       PT_INTERP segment into memory.  Otherwise glibc will choke. */
    phdrs.resize(hdr->e_phnum + 1);
    hdr->e_phnum++;
    Elf32_Phdr & phdr = phdrs[hdr->e_phnum - 1];
    phdr.p_type = PT_LOAD;
    phdr.p_offset = 0;
    phdr.p_vaddr = phdr.p_paddr = startPage;
    phdr.p_filesz = phdr.p_memsz = shift;
    phdr.p_flags = PF_R | PF_W;
    phdr.p_align = 4096;
}


string ElfFile::getSectionName(const Elf32_Shdr & shdr)
{
    return string(sectionNames.c_str() + shdr.sh_name);
}


Elf32_Shdr * ElfFile::findSection2(const SectionName & sectionName)
{
    for (unsigned int i = 1; i < hdr->e_shnum; ++i)
        if (getSectionName(shdrs[i]) == sectionName) return &shdrs[i];
    return 0;
}


Elf32_Shdr & ElfFile::findSection(const SectionName & sectionName)
{
    Elf32_Shdr * shdr = findSection2(sectionName);
    if (!shdr)
        error("cannot find section " + sectionName);
    return *shdr;
}


string & ElfFile::replaceSection(const SectionName & sectionName,
    unsigned int size)
{
    ReplacedSections::iterator i = replacedSections.find(sectionName);
    string s;
    
    if (i != replacedSections.end()) {
        s = string(i->second);
    } else {
        Elf32_Shdr & shdr = findSection(sectionName);
        s = string((char *) contents + shdr.sh_offset, shdr.sh_size);
    }
    
    s.resize(size);
    replacedSections[sectionName] = s;

    return replacedSections[sectionName];
}


void ElfFile::rewriteSections()
{
    if (replacedSections.empty()) return;

    for (ReplacedSections::iterator i = replacedSections.begin();
         i != replacedSections.end(); ++i)
        debug("replacing section `%s' with size %d\n",
            i->first.c_str(), i->second.size());

    
    /* What is the index of the last replaced section? */
    unsigned int lastReplaced = 0;
    for (unsigned int i = 1; i < hdr->e_shnum; ++i) {
        string sectionName = getSectionName(shdrs[i]);
        if (replacedSections.find(sectionName) != replacedSections.end()) {
            fprintf(stderr, "using replaced section `%s'\n", sectionName.c_str());
            lastReplaced = i;
        }
    }

    assert(lastReplaced != 0);

    debug("last replaced is %d\n", lastReplaced);
    
    /* Try to replace all section before that, as far as possible.
       Stop when we reach an irreplacable section (such as one of type
       SHT_PROGBITS).  These cannot be moved in virtual address space
       since that would invalidate absolute references to them. */
    assert(lastReplaced + 1 < shdrs.size()); /* !!! I'm lazy. */
    off_t startOffset = shdrs[lastReplaced + 1].sh_offset;
    Elf32_Addr startAddr = shdrs[lastReplaced + 1].sh_addr;
    string prevSection;
    for (unsigned int i = 1; i <= lastReplaced; ++i) {
        Elf32_Shdr & shdr(shdrs[i]);
        string sectionName = getSectionName(shdr);
        debug("looking at section `%s'\n", sectionName.c_str());
        if ((shdr.sh_type == SHT_PROGBITS && sectionName != ".interp") || prevSection == ".dynstr") {
        //if (true) {
            startOffset = shdr.sh_offset;
            startAddr = shdr.sh_addr;
            lastReplaced = i - 1;
            break;
        } else {
            if (replacedSections.find(sectionName) == replacedSections.end()) {
                debug("replacing section `%s' which is in the way\n", sectionName.c_str());
                replaceSection(sectionName, shdr.sh_size);
            }
        }
        prevSection = sectionName;
    }

    debug("first reserved offset/addr is 0x%x/0x%x\n",
        startOffset, startAddr);
    
    assert(startAddr % pageSize == startOffset % pageSize);
    Elf32_Addr firstPage = startAddr - startOffset;
    debug("first page is 0x%x\n", firstPage);
        
    /* Right now we assume that the section headers are somewhere near
       the end, which appears to be the case most of the time.
       Therefore its not accidentally overwritten by the replaced
       sections. !!!  Fix this. */
    assert(hdr->e_shoff >= startOffset);

    
    /* Compute the total space needed for the replaced sections, the
       ELF header, and the program headers. */
    off_t neededSpace = sizeof(Elf32_Ehdr) + phdrs.size() * sizeof(Elf32_Phdr);
    for (ReplacedSections::iterator i = replacedSections.begin();
         i != replacedSections.end(); ++i)
        neededSpace += roundUp(i->second.size(), 4);

    debug("needed space is %d\n", neededSpace);

    /* If we need more space at the start of the file, then grow the
       file by the minimum number of pages and adjust internal
       offsets. */
    if (neededSpace > startOffset) {

        /* We also need an additional program header, so adjust for that. */
        neededSpace += sizeof(Elf32_Phdr);
        debug("needed space is %d\n", neededSpace);
        
        unsigned int neededPages = roundUp(neededSpace - startOffset, pageSize) / pageSize;
        debug("needed pages is %d\n", neededPages);
        if (neededPages * pageSize > firstPage)
            error("virtual address space underrun!");
        
        firstPage -= neededPages * pageSize;
        startOffset += neededPages * pageSize;

        shiftFile(neededPages, firstPage);
    }


    /* Clear out the free space. */
    Elf32_Off curOff = sizeof(Elf32_Ehdr) + phdrs.size() * sizeof(Elf32_Phdr);
    memset(contents + curOff, 0, startOffset - curOff);
    

    /* Write out the replaced sections. */
    for (ReplacedSections::iterator i = replacedSections.begin();
         i != replacedSections.end(); )
    {
        string sectionName = i->first;
        debug("rewriting section `%s' to offset %d\n",
            sectionName.c_str(), curOff);
        memcpy(contents + curOff, (unsigned char *) i->second.c_str(),
            i->second.size());

        /* Update the section header for this section. */
        Elf32_Shdr & shdr = findSection(sectionName);
        shdr.sh_offset = curOff;
        shdr.sh_addr = firstPage + curOff;
        shdr.sh_size = i->second.size();
        shdr.sh_addralign = 4;

        /* If this is the .interp section, then the PT_INTERP segment
           must be sync'ed with it. */
        if (sectionName == ".interp") {
            for (int j = 0; j < phdrs.size(); ++j)
                if (phdrs[j].p_type == PT_INTERP) {
                    phdrs[j].p_offset = shdr.sh_offset;
                    phdrs[j].p_vaddr = phdrs[j].p_paddr = shdr.sh_addr;
                    phdrs[j].p_filesz = phdrs[j].p_memsz = shdr.sh_size;
                }
        }

        /* If this is the .dynamic section, then the PT_DYNAMIC segment
           must be sync'ed with it. */
        if (sectionName == ".dynamic") {
            for (int j = 0; j < phdrs.size(); ++j)
                if (phdrs[j].p_type == PT_DYNAMIC) {
                    phdrs[j].p_offset = shdr.sh_offset;
                    phdrs[j].p_vaddr = phdrs[j].p_paddr = shdr.sh_addr;
                    phdrs[j].p_filesz = phdrs[j].p_memsz = shdr.sh_size;
                }
        }

        curOff += roundUp(i->second.size(), 4);

        ++i;
        replacedSections.erase(sectionName);
    }

    assert(replacedSections.empty());
    assert(curOff == neededSpace);


    /* Rewrite the program header table. */

    /* If the is a segment for the program header table, update it.
       (According to the ELF spec, it must be the first entry.) */
    if (phdrs[0].p_type == PT_PHDR) {
        phdrs[0].p_offset = hdr->e_phoff;
        phdrs[0].p_vaddr = phdrs[0].p_paddr = firstPage + hdr->e_phoff;
        phdrs[0].p_filesz = phdrs[0].p_memsz = phdrs.size() * sizeof(Elf32_Phdr);
    }

    for (int i = 0; i < phdrs.size(); ++i)
        * ((Elf32_Phdr *) (contents + hdr->e_phoff) + i) = phdrs[i];

    /* Rewrite the section header table. */
    assert(hdr->e_shnum == shdrs.size());
    for (int i = 1; i < hdr->e_shnum; ++i)
        * ((Elf32_Shdr *) (contents + hdr->e_shoff) + i) = shdrs[i];

    /* Update all those nasty virtual addresses in the .dynamic
       section. */
    Elf32_Shdr & shdrDynamic = findSection(".dynamic");
    Elf32_Dyn * dyn = (Elf32_Dyn *) (contents + shdrDynamic.sh_offset);
    for ( ; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == DT_STRTAB)
            dyn->d_un.d_ptr = findSection(".dynstr").sh_addr;
        else if (dyn->d_tag == DT_STRSZ)
            dyn->d_un.d_val = findSection(".dynstr").sh_size;
#if 1
        else if (dyn->d_tag == DT_SYMTAB)
            dyn->d_un.d_ptr = findSection(".dynsym").sh_addr;
        else if (dyn->d_tag == DT_HASH)
            dyn->d_un.d_ptr = findSection(".hash").sh_addr;
        else if (dyn->d_tag == DT_JMPREL)
            dyn->d_un.d_ptr = findSection(".rel.plt").sh_addr;
        else if (dyn->d_tag == DT_REL) { /* !!! hack! */
            Elf32_Shdr * shdr = findSection2(".rel.dyn");
            /* no idea if this makes sense, but it was needed for some
               program*/
            if (!shdr) shdr = findSection2(".rel.got");
            if (!shdr) error("cannot find .rel.dyn or .rel.got");
            dyn->d_un.d_ptr = shdr->sh_addr;
        }
        else if (dyn->d_tag == DT_VERNEED)
            dyn->d_un.d_ptr = findSection(".gnu.version_r").sh_addr;
        else if (dyn->d_tag == DT_VERSYM)
            dyn->d_un.d_ptr = findSection(".gnu.version").sh_addr;
#endif
}


static void setSubstr(string & s, unsigned int pos, const string & t)
{
    assert(pos + t.size() <= s.size());
    copy(t.begin(), t.end(), s.begin() + pos);
}


string ElfFile::getInterpreter()
{
    Elf32_Shdr & shdr = findSection(".interp");
    return string((char *) contents + shdr.sh_offset, shdr.sh_size);
}


void ElfFile::setInterpreter(const string & newInterpreter)
{
    string & section = replaceSection(".interp", newInterpreter.size() + 1);
    setSubstr(section, 0, newInterpreter + '\0');
    changed = true;
}


static void concatToRPath(string & rpath, const string & path)
{
    if (!rpath.empty()) rpath += ":";
    rpath += path;
}


void ElfFile::modifyRPath(RPathOp op, string newRPath)
{
    Elf32_Shdr & shdrDynamic = findSection(".dynamic");

    /* !!! We assume that the virtual address in the DT_STRTAB entry
       of the dynamic section corresponds to the .dynstr section. */ 
    Elf32_Shdr & shdrDynStr = findSection(".dynstr");
    char * strTab = (char *) contents + shdrDynStr.sh_offset;

    /* Find the DT_STRTAB entry in the dynamic section. */
    Elf32_Dyn * dyn = (Elf32_Dyn *) (contents + shdrDynamic.sh_offset);
    Elf32_Addr strTabAddr = 0;
    for ( ; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == DT_STRTAB) strTabAddr = dyn->d_un.d_ptr;
    if (!strTabAddr) error("strange: no string table");

    assert(strTabAddr == shdrDynStr.sh_addr);
    
    
    /* Walk through the dynamic section, look for the RPATH entry. */
    static vector<string> neededLibs;
    dyn = (Elf32_Dyn *) (contents + shdrDynamic.sh_offset);
    Elf32_Dyn * dynRPath = 0;
    Elf32_Dyn * rpathEntry = 0;
    char * rpath = 0;
    for ( ; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_RPATH) {
            dynRPath = dyn;
            rpathEntry = dyn;
            rpath = strTab + dyn->d_un.d_val;
        }
        else if (dyn->d_tag == DT_NEEDED)
            neededLibs.push_back(string(strTab + dyn->d_un.d_val));
    }

    if (op == rpPrint) {
        printf("%s\n", rpath ? rpath : "");
        return;
    }
    
    if (op == rpShrink && !rpath) {
        debug("no RPATH to shrink\n");
        return;
    }

    
    /* For each directory in the RPATH, check if it contains any
       needed library. */
    if (op == rpShrink) {
        static vector<bool> neededLibFound(neededLibs.size(), false);

        newRPath = "";

        char * pos = rpath;
        while (*pos) {
            char * end = strchr(pos, ':');
            if (!end) end = strchr(pos, 0);

            /* Get the name of the directory. */
            string dirName(pos, end - pos);
            if (*end == ':') ++end;
            pos = end;

            /* Non-absolute entries are allowed (e.g., the special
               "$ORIGIN" hack). */
            if (dirName[0] != '/') {
                concatToRPath(newRPath, dirName);
                continue;
            }

            /* For each library that we haven't found yet, see if it
               exists in this directory. */
            int j;
            bool libFound = false;
            for (j = 0; j < neededLibs.size(); ++j)
                if (!neededLibFound[j]) {
                    string libName = dirName + "/" + neededLibs[j];
                    struct stat st;
                    if (stat(libName.c_str(), &st) == 0) {
                        neededLibFound[j] = true;
                        libFound = true;
                    }
                }

            if (!libFound)
                debug("removing directory `%s' from RPATH\n", dirName.c_str());
            else
                concatToRPath(newRPath, dirName);
        }
    }

    
    if (string(rpath ? rpath : "") == newRPath) return;

    changed = true;
    
    /* Zero out the previous rpath to prevent retained
       dependencies in Nix. */
    unsigned int rpathSize = 0;
    if (rpath) {
        rpathSize = strlen(rpath);
        memset(rpath, 'X', rpathSize);
    }

    debug("new rpath is `%s'\n", newRPath.c_str());
    
    if (newRPath.size() <= rpathSize) {
        strcpy(rpath, newRPath.c_str());
        return;
    }

    /* Grow the .dynstr section to make room for the new RPATH. */
    debug("rpath is too long, resizing...\n");

    string & newDynStr = replaceSection(".dynstr",
        shdrDynStr.sh_size + newRPath.size() + 1);
    setSubstr(newDynStr, shdrDynStr.sh_size, newRPath + '\0');

    /* Update the DT_RPATH entry. */
    if (dynRPath)
        dynRPath->d_un.d_val = shdrDynStr.sh_size;

    else {
        /* There is no DT_RPATH entry in the .dynamic section, so we
           have to grow the .dynamic section. */
        string & newDynamic = replaceSection(".dynamic",
            shdrDynamic.sh_size + sizeof(Elf32_Dyn));

        unsigned int idx = 0;
        dyn = (Elf32_Dyn *) newDynamic.c_str();
        for ( ; dyn->d_tag != DT_NULL; dyn++, idx++) ;

        debug("DT_NULL index is %d\n", idx);
        
        Elf32_Dyn newDyn;
        newDyn.d_tag = DT_RPATH;
        newDyn.d_un.d_val = shdrDynStr.sh_size;
        setSubstr(newDynamic, idx * sizeof(Elf32_Dyn),
            string((char *) &newDyn, sizeof(Elf32_Dyn)));

        newDyn.d_tag = DT_NULL;
        newDyn.d_un.d_val = 0;
        setSubstr(newDynamic, (idx + 1) * sizeof(Elf32_Dyn),
            string((char *) &newDyn, sizeof(Elf32_Dyn)));
    }
}


static bool printInterpreter = false;
static string newInterpreter;

static bool shrinkRPath = false;
static bool setRPath = false;
static bool printRPath = false;
static string newRPath;


static void patchElf()
{
    if (!printInterpreter && !printRPath)
        debug("patching ELF file `%s'\n", fileName.c_str());

    mode_t fileMode;
    
    readFile(fileName, &fileMode);

    ElfFile elfFile;

    elfFile.parse();


    if (printInterpreter)
        printf("%s\n", elfFile.getInterpreter().c_str());
    
    if (newInterpreter != "")
        elfFile.setInterpreter(newInterpreter);

    if (printRPath)
        elfFile.modifyRPath(ElfFile::rpPrint, "");

    if (shrinkRPath)
        elfFile.modifyRPath(ElfFile::rpShrink, "");
    else if (setRPath)
        elfFile.modifyRPath(ElfFile::rpSet, newRPath);
    
    
    if (elfFile.isChanged()){
        elfFile.rewriteSections();
        writeFile(fileName, fileMode);
    }
}


int main(int argc, char * * argv)
{
    if (argc <= 1) {
        fprintf(stderr, "syntax: %s\n\
  [--set-interpreter FILENAME]\n\
  [--print-interpreter]\n\
  [--set-rpath RPATH]\n\
  [--shrink-rpath]\n\
  [--print-rpath]\n\
  [--debug]\n\
  FILENAME\n", argv[0]);
        return 1;
    }

    if (getenv("PATCHELF_DEBUG") != 0) debugMode = true;

    int i;
    for (i = 1; i < argc; ++i) {
        string arg(argv[i]);
        if (arg == "--set-interpreter" || arg == "--interpreter") {
            if (++i == argc) error("missing argument");
            newInterpreter = argv[i];
        }
        else if (arg == "--print-interpreter") {
            printInterpreter = true;
        }
        else if (arg == "--shrink-rpath") {
            shrinkRPath = true;
        }
        else if (arg == "--set-rpath") {
            if (++i == argc) error("missing argument");
            setRPath = true;
            newRPath = argv[i];
        }
        else if (arg == "--print-rpath") {
            printRPath = true;
        }
        else if (arg == "--debug") {
            debugMode = true;
        }
        else break;
    }

    if (i == argc) error("missing filename");
    fileName = argv[i];

    patchElf();

    return 0;
}
