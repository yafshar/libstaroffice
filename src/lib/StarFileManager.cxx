/* -*- Mode: C++; c-default-style: "k&r"; indent-tabs-mode: nil; tab-width: 2; c-basic-offset: 2 -*- */

/* libstaroffice
* Version: MPL 2.0 / LGPLv2+
*
* The contents of this file are subject to the Mozilla Public License Version
* 2.0 (the "License"); you may not use this file except in compliance with
* the License or as specified alternatively below. You may obtain a copy of
* the License at http://www.mozilla.org/MPL/
*
* Software distributed under the License is distributed on an "AS IS" basis,
* WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
* for the specific language governing rights and limitations under the
* License.
*
* Major Contributor(s):
* Copyright (C) 2002 William Lachance (wrlach@gmail.com)
* Copyright (C) 2002,2004 Marc Maurer (uwog@uwog.net)
* Copyright (C) 2004-2006 Fridrich Strba (fridrich.strba@bluewin.ch)
* Copyright (C) 2006, 2007 Andrew Ziem
* Copyright (C) 2011, 2012 Alonso Laurent (alonso@loria.fr)
*
*
* All Rights Reserved.
*
* For minor contributions see the git repository.
*
* Alternatively, the contents of this file may be used under the terms of
* the GNU Lesser General Public License Version 2 or later (the "LGPLv2+"),
* in which case the provisions of the LGPLv2+ are applicable
* instead of those above.
*/

#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include <librevenge/librevenge.h>

#include "StarZone.hxx"

#include "StarAttribute.hxx"
#include "StarBitmap.hxx"
#include "StarDocument.hxx"
#include "StarItemPool.hxx"

#include "StarFileManager.hxx"

/** Internal: the structures of a StarFileManager */
namespace StarFileManagerInternal
{
////////////////////////////////////////
//! Internal: a structure use to read SfxMultiRecord zone of a StarFileManager
struct SfxMultiRecord {
  //! constructor
  SfxMultiRecord(StarZone &zone) : m_zone(zone), m_zoneType(0), m_zoneOpened(false), m_headerType(0), m_headerVersion(0), m_headerTag(0),
    m_actualRecord(0), m_numRecord(0), m_contentSize(0),
    m_startPos(0), m_endPos(0), m_offsetList(), m_extra("")
  {
  }
  //! try to open a zone
  bool open()
  {
    if (m_zoneOpened) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord: oops a record has been opened\n"));
      return false;
    }
    m_actualRecord=m_numRecord=0;
    m_headerType=m_headerVersion=0;
    m_headerTag=0;
    m_contentSize=0;
    m_offsetList.clear();

    STOFFInputStreamPtr input=m_zone.input();
    long pos=input->tell();
    if (!m_zone.openSfxRecord(m_zoneType)) {
      input->seek(pos, librevenge::RVNG_SEEK_SET);
      return false;
    }
    if (m_zoneType==char(0xff)) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord: oops end header\n"));
      m_extra="###emptyZone,";
      return true; /* empty zone*/
    }
    if (m_zoneType!=0) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord: find unknown header\n"));
      m_extra="###badZoneType,";
      return true;
    }

    m_zoneOpened=true;
    m_endPos=m_zone.getRecordLastPosition();
    // filerec.cxx: SfxSingleRecordReader::FindHeader_Impl
    if (input->tell()+10>m_endPos) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord::open: oops the zone seems too short\n"));
      m_extra="###zoneShort,";
      return true;
    }
    *input >> m_headerType >> m_headerVersion >> m_headerTag;
    // filerec.cxx: SfxMultiRecordReader::ReadHeader_Impl
    *input >> m_numRecord >> m_contentSize;
    m_startPos=input->tell();
    std::stringstream s;
    if (m_headerType==2) {
      // fixed size
      if (m_startPos+long(m_numRecord)*long(m_contentSize) > m_endPos) {
        STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord::open: oops the number of record seems bad\n"));
        s << "##numRecord=" << m_numRecord << ",";
        if (m_contentSize && m_endPos>m_startPos)
          m_numRecord=uint16_t((m_endPos-m_startPos)/long(m_contentSize));
        else
          m_numRecord=0;
      }
      m_extra=s.str();
      return true;
    }

    long debOffsetList=((m_headerType==3 || m_headerType==7) ? m_startPos : 0) + long(m_contentSize);
    if (debOffsetList<m_startPos || debOffsetList+4*m_numRecord > m_endPos) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord::open: can not find the version map offset\n"));
      s << "###contentCount";
      m_numRecord=0;
      m_extra=s.str();
      return true;
    }
    m_endPos=debOffsetList;
    input->seek(debOffsetList, librevenge::RVNG_SEEK_SET);
    for (uint16_t i=0; i<m_numRecord; ++i) {
      uint32_t offset;
      *input >> offset;
      m_offsetList.push_back(offset);
    }
    input->seek(m_startPos, librevenge::RVNG_SEEK_SET);
    return true;
  }
  //! try to close a zone
  void close(std::string const &wh)
  {
    if (!m_zoneOpened) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord::close: can not find any opened zone\n"));
      return;
    }
    m_zoneOpened=false;
    STOFFInputStreamPtr input=m_zone.input();
    if (input->tell()<m_endPos && input->tell()+4>=m_endPos) { // small diff is possible
      m_zone.ascii().addDelimiter(input->tell(),'|');
      input->seek(m_zone.getRecordLastPosition(), librevenge::RVNG_SEEK_SET);
    }
    else if (input->tell()==m_endPos)
      input->seek(m_zone.getRecordLastPosition(), librevenge::RVNG_SEEK_SET);
    m_zone.closeSfxRecord(m_zoneType, wh);
  }
  //! returns the header tag or -1
  int getHeaderTag() const
  {
    return !m_zoneOpened ? -1 : int(m_headerTag);
  }
  //! try to go to the new content positon
  bool getNewContent(std::string const &wh)
  {
    // SfxMultiRecordReader::GetContent
    long newPos=getLastContentPosition();
    if (newPos>=m_endPos) return false;
    STOFFInputStreamPtr input=m_zone.input();
    ++m_actualRecord;
    if (input->tell()<newPos && input->tell()+4>=newPos) { // small diff is possible
      m_zone.ascii().addDelimiter(input->tell(),'|');
      input->seek(newPos, librevenge::RVNG_SEEK_SET);
    }
    else if (input->tell()!=newPos) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord::getNewContent: find extra data\n"));
      m_zone.ascii().addPos(input->tell());
      libstoff::DebugStream f;
      f << wh << ":###extra";
      m_zone.ascii().addNote(f.str().c_str());
      input->seek(newPos, librevenge::RVNG_SEEK_SET);
    }
    if (m_headerType==7 || m_headerType==8) {
      // TODO: readtag
      input->seek(2, librevenge::RVNG_SEEK_CUR);
    }
    return true;
  }
  //! returns the last content position
  long getLastContentPosition() const
  {
    if (m_actualRecord >= m_numRecord) return m_endPos;
    if (m_headerType==2) return m_startPos+m_actualRecord*long(m_contentSize);
    if (m_actualRecord >= uint16_t(m_offsetList.size())) {
      STOFF_DEBUG_MSG(("StarFileManagerInternal::SfxMultiRecord::getLastContentPosition: argh, find unexpected index\n"));
      return m_endPos;
    }
    return m_startPos+long(m_offsetList[size_t(m_actualRecord)]>>8)-14;
  }

  //! basic operator<< ; print header data
  friend std::ostream &operator<<(std::ostream &o, SfxMultiRecord const &r)
  {
    if (!r.m_zoneOpened) {
      o << r.m_extra;
      return o;
    }
    if (r.m_headerType) o << "type=" << int(r.m_headerType) << ",";
    if (r.m_headerVersion) o << "version=" << int(r.m_headerVersion) << ",";
    if (r.m_headerTag) o << "tag=" << r.m_headerTag << ",";
    if (r.m_numRecord) o << "num[record]=" << r.m_numRecord << ",";
    if (r.m_contentSize) o << "content[size/pos]=" << r.m_contentSize << ",";
    if (!r.m_offsetList.empty()) {
      o << "offset=[";
      for (size_t i=0; i<r.m_offsetList.size(); ++i) {
        uint32_t off=r.m_offsetList[i];
        if (off&0xff)
          o << (off>>8) << ":" << (off&0xff) << ",";
        else
          o << (off>>8) << ",";
      }
      o << "],";
    }
    o << r.m_extra;
    return o;
  }
protected:
  //! the main zone
  StarZone &m_zone;
  //! the zone type
  char m_zoneType;
  //! true if a SfxRecord has been opened
  bool m_zoneOpened;
  //! the record type
  uint8_t m_headerType;
  //! the header version
  uint8_t m_headerVersion;
  //! the header tag
  uint16_t m_headerTag;
  //! the actual record
  uint16_t m_actualRecord;
  //! the number of record
  uint16_t m_numRecord;
  //! the record/content/pos size
  uint32_t m_contentSize;
  //! the start of data position
  long m_startPos;
  //! the end of data position
  long m_endPos;
  //! the list of (offset + type)
  std::vector<uint32_t> m_offsetList;
  //! extra data
  std::string m_extra;
private:
  SfxMultiRecord(SfxMultiRecord const &orig);
  SfxMultiRecord &operator=(SfxMultiRecord const &orig);
};

////////////////////////////////////////
//! Internal: the state of a StarFileManager
struct State {
  //! constructor
  State()
  {
  }
};

}

////////////////////////////////////////////////////////////
// constructor/destructor, ...
////////////////////////////////////////////////////////////
StarFileManager::StarFileManager() : m_state(new StarFileManagerInternal::State)
{
}

StarFileManager::~StarFileManager()
{
}

bool StarFileManager::readImageDocument(STOFFInputStreamPtr input, librevenge::RVNGBinaryData &data, std::string const &fileName)
{
  // see this Ole with classic bitmap format
  libstoff::DebugFile ascii(input);
  ascii.open(fileName);
  ascii.skipZone(0, input->size());

  input->seek(0, librevenge::RVNG_SEEK_SET);
  data.clear();
  if (!input->readEndDataBlock(data)) {
    STOFF_DEBUG_MSG(("StarFileManager::readImageDocument: can not read image content\n"));
    return false;
  }
#ifdef DEBUG_WITH_FILES
  libstoff::Debug::dumpFile(data, (fileName+".ppm").c_str());
#endif
  return true;
}

bool StarFileManager::readEmbeddedPicture(STOFFInputStreamPtr input, librevenge::RVNGBinaryData &data, std::string &dataType, std::string const &fileName)
{
  // see this Ole with classic bitmap format
  StarZone zone(input, fileName, "EmbeddedPicture");

  libstoff::DebugFile &ascii=zone.ascii();
  ascii.open(fileName);
  data.clear();
  dataType="";
  // impgraph.cxx: ImpGraphic::ImplReadEmbedded

  input->seek(0, librevenge::RVNG_SEEK_SET);
  libstoff::DebugStream f;
  f << "Entries(EmbeddedPicture):";
  uint32_t nId;
  int32_t nLen;

  *input>>nId;
  if (nId==0x35465347 || nId==0x47534635) { // CHECKME: order
    int32_t nType;
    uint16_t nMapMode;
    int32_t nOffsX, nOffsY, nScaleNumX, nScaleDenomX, nScaleNumY, nScaleDenomY;
    bool mbSimple;
    *input >> nType >> nLen;
    if (nType) f << "type=" << nType << ",";
    // mapmod.cxx: operator>>(..., ImplMapMode& )
    *input >> nMapMode >> nOffsX>> nOffsY;
    if (nMapMode) f << "mapMode=" << nMapMode << ",";
    *input >> nScaleNumX >> nScaleDenomX >> nScaleNumY >> nScaleDenomY >> mbSimple;
    f << "scaleX=" << nScaleNumX << "/" << nScaleDenomX << ",";
    f << "scaleY=" << nScaleNumY << "/" << nScaleDenomY << ",";
    if (mbSimple) f << "simple,";
  }
  else {
    if (nId>0x100) {
      input->seek(0, librevenge::RVNG_SEEK_SET);
      input->setReadInverted(!input->readInverted());
      *input>>nId;
    }
    if (nId) f << "type=" << nId << ",";
    int32_t nWidth, nHeight, nMapMode, nScaleNumX, nScaleDenomX, nScaleNumY, nScaleDenomY, nOffsX, nOffsY;
    *input >> nLen >> nWidth >> nHeight >> nMapMode;
    f << "size=" << nWidth << "x" << nHeight << ",";
    if (nMapMode) f << "mapMode=" << nMapMode << ",";
    *input >> nScaleNumX >> nScaleDenomX >> nScaleNumY >> nScaleDenomY;
    f << "scaleX=" << nScaleNumX << "/" << nScaleDenomX << ",";
    f << "scaleY=" << nScaleNumY << "/" << nScaleDenomY << ",";
    *input >> nOffsX >> nOffsY;
    f << "offset=" << nOffsX << "x" << nOffsY << ",";
  }
  if (nLen<10 || input->size()!=input->tell()+nLen) {
    f << "###";
    STOFF_DEBUG_MSG(("StarFileManager::readEmbeddedPicture: the length seems bad\n"));
    ascii.addPos(0);
    ascii.addNote(f.str().c_str());
    return false;
  }
  long pictPos=input->tell();
  int header=(int) input->readULong(2);
  input->seek(pictPos, librevenge::RVNG_SEEK_SET);
  std::string extension("pict");
  if (header==0x4142 || header==0x4d42) {
    dataType="image/bm";
    extension="bm";
#ifdef DEBUG_WITH_FILES
    StarBitmap bitmap;
    bitmap.readBitmap(zone, true, input->size(), data, dataType);
#endif
  }
  else if (header==0x5653) {
#ifdef DEBUG_WITH_FILES
    readSVGDI(zone);
#endif
    dataType="image/svg";
    extension="svgdi";
  }
  else if (header==0xcdd7) {
    dataType="image/wmf";
    extension="wmf";
  }
  else {
    dataType="image/pict";
    f << "###unknown";
    STOFF_DEBUG_MSG(("StarFileManager::readEmbeddedPicture: find unknown format\n"));
  }
  f << extension << ",";
  if (input->tell()==pictPos) ascii.addDelimiter(input->tell(),'|');
  ascii.addPos(0);
  ascii.addNote(f.str().c_str());
  ascii.skipZone(pictPos+4, input->size());

  input->seek(pictPos, librevenge::RVNG_SEEK_SET);
  if (!input->readEndDataBlock(data)) {
    data.clear();
    STOFF_DEBUG_MSG(("StarFileManager::readEmbeddedPicture: can not read image content\n"));
    return true;
  }
#ifdef DEBUG_WITH_FILES
  libstoff::Debug::dumpFile(data, (fileName+"."+extension).c_str());
#endif
  return true;
}

bool StarFileManager::readOleObject(STOFFInputStreamPtr input, std::string const &fileName)
{
  // see this Ole only once with PaintBrush data ?
  libstoff::DebugFile ascii(input);
  ascii.open(fileName);
  ascii.skipZone(0, input->size());

#ifdef DEBUG_WITH_FILES
  input->seek(0, librevenge::RVNG_SEEK_SET);
  librevenge::RVNGBinaryData data;
  if (!input->readEndDataBlock(data)) {
    STOFF_DEBUG_MSG(("StarFileManager::readOleObject: can not read image content\n"));
    return false;
  }
  libstoff::Debug::dumpFile(data, (fileName+".ole").c_str());
#endif
  return true;
}

bool StarFileManager::readMathDocument(STOFFInputStreamPtr input, std::string const &fileName)
{
  StarZone zone(input, fileName, "MathDocument"); // use encoding RTL_TEXTENCODING_MS_1252
  libstoff::DebugFile &ascii=zone.ascii();
  ascii.open(fileName);

  input->seek(0, librevenge::RVNG_SEEK_SET);
  libstoff::DebugStream f;
  f << "Entries(SMMathDocument):";
  // starmath_document.cxx Try3x
  uint32_t lIdent, lVersion;
  *input >> lIdent;
  if (lIdent==0x534d3330 || lIdent==0x30333034) {
    input->setReadInverted(input->readInverted());
    input->seek(0, librevenge::RVNG_SEEK_SET);
    *input >> lIdent;
  }
  if (lIdent!=0x30334d53L && lIdent!=0x34303330L) {
    STOFF_DEBUG_MSG(("StarFileManager::readMathDocument: find unexpected header\n"));
    f << "###header";
    ascii.addPos(0);
    ascii.addNote(f.str().c_str());
    return true;
  }
  *input >> lVersion;
  f << "vers=" << std::hex << lVersion << std::dec << ",";
  ascii.addPos(0);
  ascii.addNote(f.str().c_str());
  librevenge::RVNGString text;
  while (!input->isEnd()) {
    long pos=input->tell();
    int8_t cTag;
    *input>>cTag;
    f.str("");
    f << "SMMathDocument[" << char(cTag) << "]:";
    bool done=true, isEnd=false;;
    switch (cTag) {
    case 0:
      isEnd=true;
      break;
    case 'T':
      if (!zone.readString(text)) {
        done=false;
        break;
      }
      f << text.cstr();
      break;
    case 'D':
      for (int i=0; i<4; ++i) {
        if (!zone.readString(text)) {
          STOFF_DEBUG_MSG(("StarFileManager::readMathDocument: can not read a string\n"));
          f << "###string" << i << ",";
          done=false;
          break;
        }
        if (!text.empty()) f << "str" << i << "=" << text.cstr() << ",";
        if (i==1 || i==2) {
          uint32_t date, time;
          *input >> date >> time;
          f << "date" << i << "=" << date << ",";
          f << "time" << i << "=" << time << ",";
        }
      }
      break;
    case 'F': {
      // starmath_format.cxx operator>>
      uint16_t n, vLeftSpace, vRightSpace, vTopSpace, vDist, vSize;
      *input>>n>>vLeftSpace>>vRightSpace;
      f << "baseHeight=" << (n&0xff) << ",";
      if (n&0x100) f << "isTextMode,";
      if (n&0x200) f << "bScaleNormalBracket,";
      if (vLeftSpace) f << "vLeftSpace=" << vLeftSpace << ",";
      if (vRightSpace) f << "vRightSpace=" << vRightSpace << ",";
      f << "size=[";
      for (int i=0; i<=4; ++i) {
        *input>>vSize;
        f << vSize << ",";
      }
      f << "],";
      *input>>vTopSpace;
      if (vTopSpace) f << "vTopSpace=" << vTopSpace << ",";
      f << "fonts=[";
      for (int i=0; i<=6; ++i) {
        // starmath_utility.cxx operator>>(..., SmFace)
        uint32_t nData1, nData2, nData3, nData4;
        f << "[";
        if (input->isEnd() || !zone.readString(text)) {
          STOFF_DEBUG_MSG(("StarFileManager::readMathDocument: can not read a font string\n"));
          f << "###string,";
          done=false;
          break;
        }
        f << text.cstr() << ",";
        *input >> nData1 >> nData2 >> nData3 >> nData4;
        if (nData1) f << "familly=" << nData1 << ",";
        if (nData2) f << "encoding=" << nData2 << ",";
        if (nData3) f << "weight=" << nData3 << ",";
        if (nData4) f << "bold=" << nData4 << ",";
        f << "],";
      }
      f << "],";
      if (!done) break;
      if (input->tell()+21*2 > input->size()) {
        STOFF_DEBUG_MSG(("StarFileManager::readMathDocument: zone is too short\n"));
        f << "###short,";
        done=false;
        break;
      }
      f << "vDist=[";
      for (int i=0; i<=18; ++i) {
        *input >> vDist;
        if (vDist)
          f << vDist << ",";
        else
          f << "_,";
      }
      f << "],";
      *input >> n >> vDist;
      f << "vers=" << (n>>8) << ",";
      f << "eHorAlign=" << (n&0xFF) << ",";
      if (vDist) f << "bottomSpace=" << vDist << ",";
      break;
    }
    case 'S': {
      if (!zone.readString(text)) {
        STOFF_DEBUG_MSG(("StarFileManager::readMathDocument: can not read a string\n"));
        f << "###string,";
        done=false;
        break;
      }
      f << text.cstr() << ",";
      uint16_t n;
      *input>>n;
      if (n) f << "n=" << n << ",";
      break;
    }
    default:
      STOFF_DEBUG_MSG(("StarFileManager::readMathDocument: find unexpected type\n"));
      f << "###type,";
      done=false;
      break;
    }
    ascii.addPos(pos);
    ascii.addNote(f.str().c_str());
    if (!done) {
      ascii.addDelimiter(input->tell(),'|');
      break;
    }
    if (isEnd)
      break;
  }
  return true;
}

bool StarFileManager::readOutPlaceObject(STOFFInputStreamPtr input, libstoff::DebugFile &ascii)
{
  input->seek(0, librevenge::RVNG_SEEK_SET);
  libstoff::DebugStream f;
  f << "Entries(OutPlaceObject):";
  // see outplace.cxx SvOutPlaceObject::SaveCompleted
  if (input->size()<7) {
    STOFF_DEBUG_MSG(("StarFileManager::readOutPlaceObject: file is too short\n"));
    f << "###";
  }
  else {
    uint16_t len;
    uint32_t dwAspect;
    bool setExtent;
    *input>>len >> dwAspect >> setExtent;
    if (len!=7) f << "length=" << len << ",";
    if (dwAspect) f << "dwAspect=" << dwAspect << ",";
    if (setExtent) f << setExtent << ",";
    if (!input->isEnd()) {
      STOFF_DEBUG_MSG(("StarFileManager::readOutPlaceObject: find extra data\n"));
      f << "###extra";
      ascii.addDelimiter(input->tell(),'|');
    }
  }
  ascii.addPos(0);
  ascii.addNote(f.str().c_str());
  return true;
}

////////////////////////////////////////////////////////////
// small zone
////////////////////////////////////////////////////////////
bool StarFileManager::readFont(StarZone &zone)
{
  STOFFInputStreamPtr input=zone.input();
  libstoff::DebugFile &ascFile=zone.ascii();
  long pos=input->tell();
  // font.cxx:  operator>>( ..., Impl_Font& )
  libstoff::DebugStream f;
  f << "Entries(StarFont)[" << zone.getRecordLevel() << "]:";
  if (!zone.openVersionCompatHeader()) {
    STOFF_DEBUG_MSG(("StarFileManager::readFont: can not read the header\n"));
    f << "###header";
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
    return false;
  }
  long lastPos=zone.getRecordLastPosition();
  for (int i=0; i<2; ++i) {
    librevenge::RVNGString string;
    if (!zone.readString(string)||input->tell()>lastPos) {
      STOFF_DEBUG_MSG(("StarFileManager::readFont: can not read a string\n"));
      f << "###string";
      ascFile.addPos(pos);
      ascFile.addNote(f.str().c_str());
      zone.closeVersionCompatHeader("StarFont");
      return true;
    }
    if (!string.empty()) f << (i==0 ? "name" : "style") << "=" << string.cstr() << ",";
  }
  f << "size=" << input->readLong(4) << "x" << input->readLong(4) << ",";
  uint16_t eCharSet, eFamily, ePitch, eWeight, eUnderline, eStrikeOut, eItalic, eLanguage, eWidthType;
  int16_t orientation;
  *input >> eCharSet >> eFamily >> ePitch >> eWeight >> eUnderline >> eStrikeOut
         >> eItalic >> eLanguage >> eWidthType >> orientation;
  if (eCharSet) f << "eCharSet=" << eCharSet << ",";
  if (eFamily) f << "eFamily=" << eFamily << ",";
  if (ePitch) f << "ePitch=" << ePitch << ",";
  if (eWeight) f << "eWeight=" << eWeight << ",";
  if (eUnderline) f << "eUnderline=" << eUnderline << ",";
  if (eStrikeOut) f << "eStrikeOut=" << eStrikeOut << ",";
  if (eItalic) f << "eItalic=" << eItalic << ",";
  if (eLanguage) f << "eLanguage=" << eLanguage << ",";
  if (eWidthType) f << "eWidthType=" << eWidthType << ",";
  if (orientation) f << "orientation=" << orientation << ",";
  bool wordline, outline, shadow;
  uint8_t kerning;
  *input >> wordline >> outline >> shadow >> kerning;
  if (wordline) f << "wordline,";
  if (outline) f << "outline,";
  if (shadow) f << "shadow,";
  if (kerning) f << "kerning=" << (int) kerning << ",";
  if (zone.getHeaderVersion() >= 2) {
    bool vertical;
    int8_t relief;
    uint16_t cjkLanguage, emphasisMark;
    *input >> relief >> cjkLanguage >> vertical >> emphasisMark;
    if (relief) f << "relief=" << int(relief) << ",";
    if (cjkLanguage) f << "cjkLanguage=" << cjkLanguage << ",";
    if (vertical) f << "vertical,";
    if (emphasisMark) f << "emphasisMark=" << emphasisMark << ",";
  }
  ascFile.addPos(pos);
  ascFile.addNote(f.str().c_str());

  zone.closeVersionCompatHeader("StarFont");
  return true;
}

bool StarFileManager::readJobSetUp(StarZone &zone)
{
  STOFFInputStreamPtr input=zone.input();
  libstoff::DebugFile &ascFile=zone.ascii();
  long pos=input->tell();
  // sw_sw3misc.cxx: InJobSetup
  libstoff::DebugStream f;
  f << "Entries(JobSetUp)[" << zone.getRecordLevel() << "]:";
  // sfx2_printer.cxx: SfxPrinter::Create
  // jobset.cxx: JobSetup operator>>
  long lastPos=zone.getRecordLastPosition();
  int len=(int) input->readULong(2);
  f << "nLen=" << len << ",";
  if (len==0) {
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
    return true;
  }
  bool ok=false;
  int nSystem=0;
  if (input->tell()+2+64+3*32<=lastPos) {
    nSystem=(int) input->readULong(2);
    f << "system=" << nSystem << ",";
    for (int i=0; i<4; ++i) {
      long actPos=input->tell();
      int const length=(i==0 ? 64 : 32);
      std::string name("");
      for (int c=0; c<length; ++c) {
        char ch=(char)input->readULong(1);
        if (ch==0) break;
        name+=ch;
      }
      if (!name.empty()) {
        static char const *(wh[])= { "printerName", "deviceName", "portName", "driverName" };
        f << wh[i] << "=" << name << ",";
      }
      input->seek(actPos+length, librevenge::RVNG_SEEK_SET);
    }
    ok=true;
  }
  if (ok && nSystem<0xffffe) {
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());

    pos=input->tell();
    f.str("");
    f << "JobSetUp:driverData,";
    input->seek(lastPos, librevenge::RVNG_SEEK_SET);
  }
  else if (ok && input->tell()+22<=lastPos) {
    int driverDataLen=0;
    f << "nSize2=" << input->readULong(2) << ",";
    f << "nSystem2=" << input->readULong(2) << ",";
    driverDataLen=(int) input->readULong(4);
    f << "nOrientation=" << input->readULong(2) << ",";
    f << "nPaperBin=" << input->readULong(2) << ",";
    f << "nPaperFormat=" << input->readULong(2) << ",";
    f << "nPaperWidth=" << input->readULong(4) << ",";
    f << "nPaperHeight=" << input->readULong(4) << ",";

    if (driverDataLen && input->tell()+driverDataLen<=lastPos) {
      ascFile.addPos(input->tell());
      ascFile.addNote("JobSetUp:driverData");
      input->seek(driverDataLen, librevenge::RVNG_SEEK_CUR);
    }
    else if (driverDataLen)
      ok=false;
    if (ok) {
      ascFile.addPos(pos);
      ascFile.addNote(f.str().c_str());
      pos=input->tell();
      f.str("");
      f << "JobSetUp[values]:";
      if (nSystem==0xfffe) {
        librevenge::RVNGString text;
        while (input->tell()<lastPos) {
          for (int i=0; i<2; ++i) {
            if (!zone.readString(text)) {
              f << "###string,";
              ok=false;
              break;
            }
            f << text.cstr() << (i==0 ? ':' : ',');
          }
          if (!ok)
            break;
        }
      }
      else if (input->tell()<lastPos) {
        ascFile.addPos(input->tell());
        ascFile.addNote("JobSetUp:driverData");
        input->seek(lastPos, librevenge::RVNG_SEEK_SET);
      }
    }
  }

  ascFile.addPos(pos);
  ascFile.addNote(f.str().c_str());
  return true;
}

bool StarFileManager::readEditTextObject(StarZone &zone, long lastPos, StarDocument &doc)
{
  // svx_editobj.cxx EditTextObject::Create
  STOFFInputStreamPtr input=zone.input();
  long pos=input->tell();
  libstoff::DebugFile &ascFile=zone.ascii();
  libstoff::DebugStream f;

  f << "Entries(EditTextObject):";
  uint16_t nWhich;
  uint32_t nStructSz;
  *input >> nWhich >> nStructSz;
  f << "structSz=" << nStructSz << ",";
  f << "nWhich=" << nWhich << ",";
  if ((nWhich != 0x22 && nWhich !=0x31) || pos+6+long(nStructSz)>lastPos) {
    STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: bad which/structSz\n"));
    f << "###";
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
    return false;
  }
  lastPos=pos+6+long(nStructSz);
  // BinTextObject::CreateData or BinTextObject::CreateData300
  uint16_t version=0;
  bool ownPool=true;
  if (nWhich==0x31) {
    *input>>version >> ownPool;
    if (version) f << "vers=" << version << ",";
  }
  ascFile.addPos(pos);
  ascFile.addNote(f.str().c_str());

  pos=input->tell();
  shared_ptr<StarItemPool> pool;
  if (!ownPool) pool=doc.findItemPool(StarItemPool::T_EditEnginePool, true); // checkme
  if (!pool) pool=doc.getNewItemPool(StarItemPool::T_EditEnginePool);
  if (ownPool && !pool->read(zone)) {
    STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: can not read a pool\n"));
    ascFile.addPos(pos);
    ascFile.addNote("EditTextObject:###pool");
    input->seek(lastPos, librevenge::RVNG_SEEK_SET);
    return true;
  }
  pos=input->tell();
  f.str("");
  f << "EditTextObject:";
  uint16_t charSet=0;
  uint32_t nPara;
  if (nWhich==0x31) {
    uint16_t numPara;
    *input>>charSet >> numPara;
    nPara=numPara;
  }
  else
    *input>> nPara;
  if (charSet)  f << "char[set]=" << charSet << ",";
  if (nPara) f << "nPara=" << nPara << ",";
  ascFile.addPos(pos);
  ascFile.addNote(f.str().c_str());

  for (int i=0; i<int(nPara); ++i) {
    pos=input->tell();
    f.str("");
    f << "EditTextObject[para" << i << "]:";
    for (int j=0; j<2; ++j) {
      librevenge::RVNGString text;
      if (!zone.readString(text) || input->tell()>lastPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: can not read a strings\n"));
        f << "###strings,";
        ascFile.addPos(pos);
        ascFile.addNote(f.str().c_str());
        input->seek(lastPos, librevenge::RVNG_SEEK_SET);
        return true;
      }
      else if (!text.empty())
        f << (j==0 ? "name" : "style") << "=" << text.cstr() << ",";
    }
    uint16_t styleFamily;
    *input >> styleFamily;
    if (styleFamily) f << "styleFam=" << styleFamily << ",";
    std::vector<STOFFVec2i> limits;
    limits.push_back(STOFFVec2i(3989, 4033)); // EE_PARA_START 4033: EE_CHAR_END
    if (!doc.readItemSet(zone, limits, lastPos, pool.get(), false)) {
      STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: can not read item list\n"));
      f << "###item list,";
      ascFile.addPos(pos);
      ascFile.addNote(f.str().c_str());
      input->seek(lastPos, librevenge::RVNG_SEEK_SET);
      return true;
    }
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());

    pos=input->tell();
    f.str("");
    f << "EditTextObjectA[para" << i << "]:";
    uint32_t nAttr;
    if (nWhich==0x22)
      *input>>nAttr;
    else {
      uint16_t nAttr16;
      *input>>nAttr16;
      nAttr=nAttr16;
    }
    if (input->tell()+long(nAttr)*8 > lastPos) {
      STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: can not read attrib list\n"));
      f << "###attrib list,";
      ascFile.addPos(pos);
      ascFile.addNote(f.str().c_str());
      input->seek(lastPos, librevenge::RVNG_SEEK_SET);
      return true;
    }
    f << "attrib=[";
    for (int j=0; j<int(nAttr); ++j) { // checkme, probably bad
      uint16_t which, start, end, surrogate;
      *input >> which;
      if (nWhich==0x22) *input >> surrogate;
      *input >> start >> end;
      if (nWhich!=0x22) *input >> surrogate;
      f << "wh=" << which << ":";
      f << "pos=" << start << "x" << end << ",";
      f << "surrogate=" << surrogate << ",";
    }
    f << "],";
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
  }

  pos=input->tell();
  f.str("");
  f << "EditTextObject:";
  if (nWhich==0x22) {
    uint16_t marker;
    *input >> marker;
    if (marker==0x9999) {
      *input >> charSet;
      if (charSet)  f << "char[set]=" << charSet << ",";
    }
  }
  if (version>=400)
    f << "tmpMetric=" << input->readULong(2) << ",";
  if (version>=600) {
    f << "userType=" << input->readULong(2) << ",";
    f << "objSettings=" << input->readULong(4) << ",";
  }
  if (version>=601)
    f << "vertical=" << input->readULong(1) << ",";
  if (version>=602) {
    f << "scriptType=" << input->readULong(2) << ",";
    bool unicodeString;
    *input >> unicodeString;
    f << "strings=[";
    for (int i=0; unicodeString && i<int(nPara); ++i) {
      for (int s=0; s<2; ++s) {
        librevenge::RVNGString text;
        if (!zone.readString(text) || input->tell()>lastPos) {
          STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: can not read a strings\n"));
          f << "###strings,";
          break;
        }
        if (text.empty())
          f << "_,";
        else
          f << text.cstr() << ",";
      }
    }
    f << "],";
  }
  if (input->tell()!=lastPos) {
    STOFF_DEBUG_MSG(("StarFileManager::readEditTextObject: find extra data\n"));
    f << "###";
    input->seek(lastPos, librevenge::RVNG_SEEK_SET);
  }
  if (pos!=lastPos) {
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
  }
  return true;
}

bool StarFileManager::readSVGDI(StarZone &zone)
{
  STOFFInputStreamPtr input=zone.input();
  libstoff::DebugFile &ascFile=zone.ascii();
  long pos=input->tell();
  long lastPos=zone.getRecordLevel()==0 ? input->size() : zone.getRecordLastPosition();
  // cvtsvm.cxx: SVMConverter::ImplConvertFromSVM1
  libstoff::DebugStream f;
  f << "Entries(ImageSVGDI)[" << zone.getRecordLevel() << "]:";
  std::string code;
  for (int i=0; i<5; ++i) code+=(char) input->readULong(1);
  if (code!="SVGDI") {
    input->seek(0, librevenge::RVNG_SEEK_SET);
    return false;
  }
  uint16_t sz;
  int16_t version;
  int32_t width, height;
  *input >> sz >> version >> width >> height;
  long endPos=pos+5+sz;
  if (version!=200 || sz<42 || endPos>lastPos) {
    STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: find unknown version\n"));
    f << "###";
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
    return false;
  }
  f << "size=" << width << "x" << height << ",";
  // map mode
  int16_t unit;
  int32_t orgX, orgY, nXNum, nXDenom, nYNum, nYDenom;
  *input >> unit >> orgX >> orgY >> nXNum >> nXDenom >> nYNum >> nYDenom;
  if (unit) f << "unit=" << unit << ",";
  f << "orig=" << orgX << "x" << orgY << ",";
  f << "x=" << nXNum << "/" << nXDenom << ",";
  f << "y=" << nYNum << "/" << nYDenom << ",";

  int32_t nActions;
  *input >> nActions;
  f << "actions=" << nActions << ",";
  if (input->tell()!=endPos)
    ascFile.addDelimiter(input->tell(),'|');
  ascFile.addPos(pos);
  ascFile.addNote(f.str().c_str());
  input->seek(endPos, librevenge::RVNG_SEEK_SET);
  uint32_t nUnicodeCommentActionNumber=0;
  for (int32_t i=0; i<nActions; ++i) {
    pos=input->tell();
    f.str("");
    f << "ImageSVGDI[" << i << "]:";
    int16_t type;
    int32_t nActionSize;
    *input>>type>>nActionSize;
    long endDataPos=pos+2+nActionSize;
    if (nActionSize<4 || endDataPos>lastPos) {
      STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad size\n"));
      f << "###";
      ascFile.addPos(pos);
      ascFile.addNote(f.str().c_str());
      input->seek(pos, librevenge::RVNG_SEEK_SET);
      break;
    }
    unsigned char col[3];
    STOFFColor color;
    int32_t nTmp, nTmp1;
    librevenge::RVNGString text;
    switch (type) {
    case 1:
      f << "pixel=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      for (int c=0; c<3; ++c) col[c]=(unsigned char)(input->readULong(2)>>8);
      f << "col=" << STOFFColor(col[0],col[1],col[2]) << ",";
      break;
    case 2:
      f << "point=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 3:
      f << "line=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 4:
      f << "rect=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      *input >> nTmp >> nTmp1;
      if (nTmp || nTmp1) f << "round=" << nTmp << "x" << nTmp1 << ",";
      break;
    case 5:
      f << "ellipse=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 6:
    case 7:
      f << (type==6 ? "arc" : "pie")<< "=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      f << "pt1=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      f << "pt2=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 8:
    case 9:
      f << (type==8 ? "rect[invert]" : "rect[highlight") << "="
        << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 10:
    case 11:
      f << (type==10 ? "polyline" : "polygon") << ",";
      *input >> nTmp;
      if (nTmp<0 || input->tell()+8*nTmp>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad number of points\n"));
        f << "###nPts=" << nTmp << ",";
        break;
      }
      f << "pts=[";
      for (int pt=0; pt<int(nTmp); ++pt) f << input->readLong(4) << "x" << input->readLong(4) << ",";
      f << "],";
      break;
    case 12:
    case 1024:
    case 1025:
    case 1030:
      f << (type==12 ? "polypoly" : type==1024 ? "transparent[comment]" :
            type==1025 ? "hatch[comment]" : "gradient[comment]") << ",";
      *input >> nTmp;
      for (int poly=0; poly<int(nTmp); ++poly) {
        *input >> nTmp1;
        if (nTmp1<0 || input->tell()+8*nTmp1>endDataPos) {
          STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad number of points\n"));
          f << "###poly[nPts=" << nTmp1 << "],";
          break;
        }
        f << "poly" << poly << "=[";
        for (int pt=0; pt<int(nTmp1); ++pt) f << input->readLong(4) << "x" << input->readLong(4) << ",";
        f << "],";
      }
      if (type==1024) {
        f << "nTrans=" << input->readULong(2) << ",";
        f << "nComment=" << input->readULong(4) << ",";
      }
      if (type==1025) {
        f << "style=" << input->readULong(2) << ",";
        for (int c=0; c<3; ++c) col[c]=(unsigned char)(input->readULong(2)>>8);
        f << "col=" << STOFFColor(col[0],col[1],col[2]) << ",";
        f << "distance=" << input->readLong(4) << ",";
        f << "nComment=" << input->readULong(4) << ",";
      }
      if (type==1030) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: reading gradient is not implemented\n"));
        f << "###gradient+following";
        input->seek(endDataPos, librevenge::RVNG_SEEK_SET);
      }
      break;
    case 13:
    case 15: {
      f << (type==13 ? "text" : "stretch") << ",";
      f << "pos=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      int32_t nIndex, nLen;
      *input>>nIndex>>nLen >> nTmp;
      if (nIndex) f << "index=" << nIndex << ",";
      if (nLen) f << "len=" << nLen << ",";
      if (type==15) f << "nWidth=" << input->readLong(4) << ",";
      if (nTmp<0 || input->tell()+nTmp>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad string\n"));
        f << "###string";
        break;
      }
      text.clear();
      for (int c=0; c<int(nTmp); ++c) text.append((char) input->readULong(1));
      input->seek(1, librevenge::RVNG_SEEK_CUR);
      f << text.cstr() << ",";
      if (nUnicodeCommentActionNumber!=(uint32_t) i) break;
      uint16_t type1;
      uint32_t len;
      *input >> type1 >> len;
      if (long(len)<4 || input->tell()+(long)len>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad string\n"));
        f << "###unicode";
        break;
      }
      if (type1==1032) {
        text.clear();
        int nUnicode=int(len-4)/2;
        for (int c=0; c<nUnicode; ++c) text.append((char) input->readULong(2));
        f << text.cstr() << ",";
      }
      else {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: unknown data\n"));
        f << "###unknown";
        input->seek(long(len)-4, librevenge::RVNG_SEEK_CUR);
      }
      break;
    }
    case 14: {
      f << "text[array],";
      f << "pos=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      int32_t nIndex, nLen, nAryLen;
      *input>>nIndex>>nLen >> nTmp >> nAryLen;
      if (nTmp<0 || nAryLen<0 || input->tell()+nTmp+4*nAryLen>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad string\n"));
        f << "###string";
        break;
      }
      text.clear();
      for (int c=0; c<int(nTmp); ++c) text.append((char) input->readULong(1));
      input->seek(1, librevenge::RVNG_SEEK_CUR);
      f << text.cstr() << ",";
      f << "ary=[";
      for (int ary=0; ary<int(nAryLen); ++ary) f << input->readLong(4) << ",";
      f << "],";
      if (nUnicodeCommentActionNumber!=(uint32_t) i) break;
      uint16_t type1;
      uint32_t len;
      *input >> type1 >> len;
      if (long(len)<4 || input->tell()+(long)len>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad string\n"));
        f << "###unicode";
        break;
      }
      if (type1==1032) {
        text.clear();
        int nUnicode=int(len-4)/2;
        for (int c=0; c<nUnicode; ++c) text.append((char) input->readULong(2));
        f << text.cstr() << ",";
      }
      else {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: unknown data\n"));
        f << "###unknown";
        input->seek(long(len)-4, librevenge::RVNG_SEEK_CUR);
      }

      break;
    }
    case 16:
      STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: reading icon is not implemented\n"));
      f << "###icon";
      break;
    case 17:
    case 18:
    case 32: {
      f << (type==17 ? "bitmap" : type==18 ? "bitmap[scale]" : "bitmap[scale2]");
      f << "pos=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      if (type>=17) f << "scale=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      if (type==32) {
        f << "pos2=" << input->readLong(4) << "x" << input->readLong(4) << ",";
        f << "scale2=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      }
      StarBitmap bitmap;
      librevenge::RVNGBinaryData data;
      std::string dataType;
      if (!bitmap.readBitmap(zone, false, endDataPos, data, dataType))
        f << "###bitmap,";
    }
    case 19:
      f << "pen,";
      for (int c=0; c<3; ++c) col[c]=(unsigned char)(input->readULong(2)>>8);
      color=STOFFColor(col[0],col[1],col[2]);
      if (!color.isBlack()) f << "col=" << color << ",";
      f << "penWidth=" << input->readULong(4) << ",";
      f << "penStyle=" << input->readULong(2) << ",";
      break;
    case 20: {
      f << "font,";
      for (int c=0; c<2; ++c) {
        for (int j=0; j<3; ++j) col[j]=(unsigned char)(input->readULong(2)>>8);
        color=STOFFColor(col[0],col[1],col[2]);
        if ((c==1&&!color.isWhite()) || (c==0&&!color.isBlack()))
          f << (c==0 ? "col" : "col[fill]") << "=" << color << ",";
      }
      long actPos=input->tell();
      if (actPos+62>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: the zone seems too short\n"));
        f << "###short";
        break;
      }
      std::string name("");
      for (int c=0; c<32; ++c) {
        char ch=(char) input->readULong(1);
        if (!ch) break;
        name+=ch;
      }
      f << name << ",";
      input->seek(actPos+32, librevenge::RVNG_SEEK_SET);
      f << "size=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      int16_t nCharSet, nFamily, nPitch, nAlign, nWeight, nUnderline, nStrikeout, nCharOrient, nLineOrient;
      bool bItalic, bOutline, bShadow, bTransparent;
      *input >> nCharSet >> nFamily >> nPitch >> nAlign >> nWeight >> nUnderline >> nStrikeout >> nCharOrient >> nLineOrient;
      if (nCharSet) f << "char[set]=" << nCharSet << ",";
      if (nFamily) f << "family=" << nFamily << ",";
      if (nPitch) f << "pitch=" << nPitch << ",";
      if (nAlign) f << "align=" << nAlign << ",";
      if (nWeight) f << "weight=" << nWeight << ",";
      if (nUnderline) f << "underline=" << nUnderline << ",";
      if (nStrikeout) f << "strikeout=" << nStrikeout << ",";
      if (nCharOrient) f << "charOrient=" << nCharOrient << ",";
      if (nLineOrient) f << "lineOrient=" << nLineOrient << ",";
      *input >> bItalic >> bOutline >> bShadow >> bTransparent;
      if (bItalic) f << "italic,";
      if (bOutline) f << "outline,";
      if (bShadow) f << "shadow,";
      if (bTransparent) f << "transparent,";
      break;
    }
    case 21: // unsure
    case 22:
      f << (type==21 ? "brush[back]" : "brush[fill]") << ",";
      for (int j=0; j<3; ++j) col[j]=(unsigned char)(input->readULong(2)>>8);
      f << STOFFColor(col[0],col[1],col[2]) << ",";
      input->seek(6, librevenge::RVNG_SEEK_CUR); // unknown
      f << "style=" << input->readLong(2) << ",";
      input->seek(2, librevenge::RVNG_SEEK_CUR); // unknown
      break;
    case 23:
      f << "map[mode],";
      *input >> unit >> orgX >> orgY >> nXNum >> nXDenom >> nYNum >> nYDenom;
      if (unit) f << "unit=" << unit << ",";
      f << "orig=" << orgX << "x" << orgY << ",";
      f << "x=" << nXNum << "/" << nXDenom << ",";
      f << "y=" << nYNum << "/" << nYDenom << ",";
      break;
    case 24: {
      f << "clip[region],";
      int16_t clipType, bIntersect;
      *input >> clipType >> bIntersect;
      f << "rect=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      if (bIntersect) f << "intersect,";
      switch (clipType) {
      case 0:
        break;
      case 1:
        f << "rect2=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
          << input->readLong(4) << "x" << input->readLong(4) << ",";
        break;
      case 2:
        *input >> nTmp;
        if (nTmp<0 || input->tell()+8*nTmp>endDataPos) {
          STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad number of points\n"));
          f << "###nPts=" << nTmp << ",";
          break;
        }
        f << "poly=[";
        for (int pt=0; pt<int(nTmp); ++pt) f << input->readLong(4) << "x" << input->readLong(4) << ",";
        f << "],";
        break;
      case 3:
        *input >> nTmp;
        for (int poly=0; poly<int(nTmp); ++poly) {
          *input >> nTmp1;
          if (nTmp1<0 || input->tell()+8*nTmp1>endDataPos) {
            STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: bad number of points\n"));
            f << "###poly[nPts=" << nTmp1 << "],";
            break;
          }
          f << "poly" << poly << "=[";
          for (int pt=0; pt<int(nTmp1); ++pt) f << input->readLong(4) << "x" << input->readLong(4) << ",";
          f << "],";
        }
        break;
      default:
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: find unknown clip type\n"));
        f << "###type=" << clipType << ",";
        break;
      }
      break;
    }
    case 25:
      f << "raster=" << input->readULong(2) << ","; // 1 invert, 4,5: xor other paint
      break;
    case 26:
      f << "push,";
      break;
    case 27:
      f << "pop,";
      break;
    case 28:
      f << "clip[move]=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 29:
      f << "clip[rect]=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      break;
    case 30: // checkme
    case 1029: {
      f << (type==30 ? "mtf" : "floatComment") << ",";;
      // gdimtf.cxx operator>>(... GDIMetaFile )
      std::string name("");
      for (int c=0; c<6; ++c) name+=(char) input->readULong(1);
      if (name!="VCLMTF") {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: find unexpected header\n"));
        f << "###name=" << name << ",";
        break;
      }
      if (!zone.openVersionCompatHeader()) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: can not open compat header\n"));
        f << "###compat,";
        break;
      }
      f << "compress=" << input->readULong(4) << ",";
      // map mode
      *input >> unit >> orgX >> orgY >> nXNum >> nXDenom >> nYNum >> nYDenom;
      f << "map=[";
      if (unit) f << "unit=" << unit << ",";
      f << "orig=" << orgX << "x" << orgY << ",";
      f << "x=" << nXNum << "/" << nXDenom << ",";
      f << "y=" << nYNum << "/" << nYDenom << ",";
      f << "],";
      f << "size=" << input->readULong(4) << ",";
      uint32_t nCount;
      *input >> nCount;
      if (nCount) f << "nCount=" << nCount << ",";
      if (input->tell()!=zone.getRecordLastPosition()) {
        // for (int act=0; act<nCount; ++act) MetaAction::ReadMetaAction();
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: reading mtf action zones is not implemented\n"));
        ascFile.addPos(input->tell());
        ascFile.addNote("ImageSVGDI:###listMeta");
        input->seek(zone.getRecordLastPosition(), librevenge::RVNG_SEEK_SET);
      }
      zone.closeVersionCompatHeader("ImageSVGDI");
      if (type!=30) {
        f << "orig=" << input->readLong(4) << "x" << input->readLong(4) << ",";
        f << "sz=" << input->readULong(4) << ",";
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: reading gradient is not implemented\n"));
        f << "###gradient+following";
        input->seek(endDataPos, librevenge::RVNG_SEEK_SET);
      }
      break;
    }
    case 33: {
      f << "gradient,";
      f << "rect=" << input->readLong(4) << "x" << input->readLong(4) << "<->"
        << input->readLong(4) << "x" << input->readLong(4) << ",";
      f << "style=" << input->readULong(2) << ",";
      for (int c=0; c<2; ++c) {
        for (int j=0; j<3; ++j) col[j]=(unsigned char)(input->readULong(2)>>8);
        color=STOFFColor(col[0],col[1],col[2]);
        f << "col" << c << "=" << color << ",";
      }
      f << "angle=" << input->readLong(2) << ",";
      f << "border=" << input->readLong(2) << ",";
      f << "offs=" << input->readLong(2) << "x" << input->readLong(2) << ",";
      f << "intensity=" << input->readLong(2) << "<->" << input->readLong(2) << ",";
      break;
    }
    case 1026:
      f << "refpoint[comment],";
      f << "pt=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      f << "set=" << input->readULong(1) << ",";
      f << "nComments=" << input->readULong(4) << ",";
      break;
    case 1027:
      f << "textline[color,comment],";
      for (int j=0; j<3; ++j) col[j]=(unsigned char)(input->readULong(2)>>8);
      f << "col=" << STOFFColor(col[0],col[1],col[2]) << ",";
      f << "set=" << input->readULong(1) << ",";
      f << "nComments=" << input->readULong(4) << ",";
      break;
    case 1028:
      f << "textline[comment],";
      f << "pt=" << input->readLong(4) << "x" << input->readLong(4) << ",";
      f << "width=" << input->readLong(4) << ",";
      f << "strikeOut=" << input->readULong(4) << ",";
      f << "underline=" << input->readULong(4) << ",";
      f << "nComments=" << input->readULong(4) << ",";
      break;
    case 1031: {
      f << "comment[comment],";
      if (!zone.readString(text)) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: can not read the text\n"));
        f << "###text,";
        break;
      }
      f << text.cstr() << ",";
      f << "value=" << input->readULong(4) << ",";
      long size=input->readLong(4);
      if (size<0 || input->tell()+size+4>endDataPos) {
        STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: size seems bad\n"));
        f << "###size=" << size << ",";
        break;
      }
      if (size) {
        f << "###unknown,";
        ascFile.addDelimiter(input->tell(),'|');
        input->seek(size, librevenge::RVNG_SEEK_CUR);
      }
      f << "nComments=" << input->readULong(4) << ",";
      break;
    }
    case 1032:
      f << "unicode[next],";
      nUnicodeCommentActionNumber=uint32_t(i)+1;
      break;
    default:
      STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: find unimplement type\n"));
      f << "###type=" << type << ",";
      input->seek(endDataPos, librevenge::RVNG_SEEK_SET);
      break;
    }
    if (input->tell()!=endDataPos) {
      STOFF_DEBUG_MSG(("StarFileManager::readSVGDI: find extra data\n"));
      f << "###extra,";
      ascFile.addDelimiter(input->tell(),'|');
    }
    ascFile.addPos(pos);
    ascFile.addNote(f.str().c_str());
    input->seek(endDataPos, librevenge::RVNG_SEEK_SET);
  }

  return true;
}

// vim: set filetype=cpp tabstop=2 shiftwidth=2 cindent autoindent smartindent noexpandtab: