﻿/*
 * (C) 2016 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "SubtitlesProvider.h"
#include "mplayerc.h"
#include "MediaInfo/library/Source/ThirdParty/base64/base64.h"
#include "tinyxml2/library/tinyxml2.h"
#include "rapidjson/include/rapidjson/document.h"

#define GUESSED_NAME_POSTFIX " (*)"

using namespace SubtitlesProvidersUtils;

/******************************************************************************
** Register providers
******************************************************************************/
void SubtitlesProviders::RegisterProviders()
{
    Register<OpenSubtitles>(this);
    Register<podnapisi>(this);
    Register<titlovi>(this);
    Register<SubDB>(this);
    Register<ysubs>(this);
}

#define CheckAbortAndReturn() { if (IsAborting()) return SR_ABORTED; }

/******************************************************************************
** OpenSubtitles
******************************************************************************/

void OpenSubtitles::Initialize()
{
    xmlrpc = std::make_unique<XmlRpcClient>("http://api.opensubtitles.org/xml-rpc");
    xmlrpc->setIgnoreCertificateAuthority();
}

SRESULT OpenSubtitles::Login(const std::string& sUserName, const std::string& sPassword)
{
    if (xmlrpc) {
        XmlRpcValue args, result;
        args[0] = sUserName;
        args[1] = sPassword;
        args[2] = "en";
        std::string strUA = UserAgent();
        args[3] = strUA.c_str(); // Test with "OSTestUserAgent"
        if (!xmlrpc->execute("LogIn", args, result)) { return SR_FAILED; }
        token = result["token"];
    }
    return token.valid() ? SR_SUCCEEDED : SR_FAILED;
}

SRESULT OpenSubtitles::Hash(SubtitlesInfo& pFileInfo)
{
    pFileInfo.fileHash = StringFormat("%016I64x", GenerateOSHash(pFileInfo));
    TRACE(_T("%S::Hash = %S\n"), Name().c_str(), pFileInfo.fileHash.c_str());
    return SR_SUCCEEDED;
}

SRESULT OpenSubtitles::Search(const SubtitlesInfo& pFileInfo)
{
    std::string languages(LanguagesISO6392());
    XmlRpcValue args, result;
    args[0] = token;
    args[1][0]["sublanguageid"] = !languages.empty() ? languages : "all";
    args[1][0]["moviehash"] = pFileInfo.fileHash;
    args[1][0]["moviebytesize"] = std::to_string(pFileInfo.fileSize);
    //args[1][1]["sublanguageid"] = !languages.empty() ? languages : "all";
    //args[1][1]["tag"] = pFileInfo.fileName + "." + pFileInfo.fileExtension;
    args[2]["limit"] = 500;

    if (!xmlrpc->execute("SearchSubtitles", args, result)) { return SR_FAILED; }

    if (result["data"].getType() != XmlRpcValue::Type::TypeArray) { return SR_FAILED; }

    int nCount = result["data"].size();
    for (int i = 0; i < nCount; ++i) {
        CheckAbortAndReturn();
        XmlRpcValue& data(result["data"][i]);
        SubtitlesInfo pSubtitlesInfo;
        pSubtitlesInfo.id = (const char*)data["IDSubtitleFile"];
        pSubtitlesInfo.discNumber = data["SubActualCD"];
        pSubtitlesInfo.discCount = data["SubSumCD"];
        pSubtitlesInfo.fileExtension = (const char*)data["SubFormat"];
        pSubtitlesInfo.languageCode = (const char*)data["ISO639"]; //"SubLanguageID"
        pSubtitlesInfo.languageName = (const char*)data["LanguageName"];
        pSubtitlesInfo.downloadCount = data["SubDownloadsCnt"];

        pSubtitlesInfo.fileName = (const char*)data["SubFileName"];
        regexResult results;
        stringMatch("\"([^\"]+)\" (.+)", (const char*)data["MovieName"], results);
        if (!results.empty()) {
            pSubtitlesInfo.title = results[0];
            pSubtitlesInfo.title2 = results[1];
        } else {
            pSubtitlesInfo.title = (const char*)data["MovieName"];
        }
        pSubtitlesInfo.year = (int)data["MovieYear"] == 0 ? -1 : (int)data["MovieYear"];
        pSubtitlesInfo.seasonNumber = (int)data["SeriesSeason"] == 0 ? -1 : (int)data["SeriesSeason"];
        pSubtitlesInfo.episodeNumber = (int)data["SeriesEpisode"] == 0 ? -1 : (int)data["SeriesEpisode"];
        pSubtitlesInfo.hearingImpaired = data["SubHearingImpaired"];
        pSubtitlesInfo.url = (const char*)data["SubtitlesLink"];
        pSubtitlesInfo.releaseName = (const char*)data["MovieReleaseName"];
        pSubtitlesInfo.imdbid = (const char*)data["IDMovieImdb"];
        pSubtitlesInfo.corrected = (int)data["SubBad"] ? -1 : 0;
        Set(pSubtitlesInfo);
    }
    return SR_SUCCEEDED;
}

SRESULT OpenSubtitles::Download(SubtitlesInfo& pSubtitlesInfo)
{
    XmlRpcValue args, result;
    args[0] = token;
    args[1][0] = pSubtitlesInfo.id;
    if (!xmlrpc->execute("DownloadSubtitles", args, result)) { return SR_FAILED; }

    if (result["data"].getType() != XmlRpcValue::Type::TypeArray) { return SR_FAILED; }

    pSubtitlesInfo.fileContents = Base64::decode(std::string(result["data"][0]["data"]));
    return SR_SUCCEEDED;
}

SRESULT OpenSubtitles::Upload(const SubtitlesInfo& pSubtitlesInfo)
{
    XmlRpcValue args, result;
    args[0] = token;

    //TODO: Ask  how to obtain commented values !!!
    args[1]["cd1"]["subhash"] = StringToHash(pSubtitlesInfo.fileContents, CALG_MD5);
    args[1]["cd1"]["subfilename"] = pSubtitlesInfo.fileName + ".srt";
    args[1]["cd1"]["moviehash"] = pSubtitlesInfo.fileHash;
    args[1]["cd1"]["moviebytesize"] = (int)pSubtitlesInfo.fileSize;
    //args[1]["cd1"]["movietimems"];
    //args[1]["cd1"]["movieframes"];
    //args[1]["cd1"]["moviefps"];
    args[1]["cd1"]["moviefilename"] = pSubtitlesInfo.fileName + "." + pSubtitlesInfo.fileExtension;

    CheckAbortAndReturn();
    if (!xmlrpc->execute("TryUploadSubtitles", args, result)) { return SR_FAILED; }
    CheckAbortAndReturn();

    if ((int)result["alreadyindb"] == 1) {
        return SR_EXISTS;
    } else if ((int)result["alreadyindb"] == 0) {
        // We need imdbid to proceed
        if (result["data"].getType() == XmlRpcValue::Type::TypeArray) {
            args[1]["baseinfo"]["idmovieimdb"] = result["data"][0]["IDMovieImdb"];
        } else if (!pSubtitlesInfo.imdbid.empty()) {
            args[1]["baseinfo"]["idmovieimdb"] = pSubtitlesInfo.imdbid;
        } else {
            std::string title(StringReplace(pSubtitlesInfo.title, "and", "&"));
            if (!args[1]["baseinfo"]["idmovieimdb"].valid()) {
                XmlRpcValue _args, _result;
                _args[0] = token;
                _args[1][0] = pSubtitlesInfo.fileHash;
                if (!xmlrpc->execute("CheckMovieHash", _args, _result)) { return SR_FAILED; }

                if (_result["data"].getType() == XmlRpcValue::Type::TypeStruct) {
                    //regexResults results;
                    //stringMatch("\"(.+)\" (.+)", (const char*)data["MovieName"], results);
                    //if (!results.empty()) {
                    //    pSubtitlesInfo.title = results[0][0];
                    //    pSubtitlesInfo.title2 = results[0][1];
                    //} else {
                    //    pSubtitlesInfo.title = (const char*)data["MovieName"];
                    //}
                    regexResults results;
                    stringMatch("\"(.+)\" .+|(.+)", StringReplace((const char*)_result["data"][pSubtitlesInfo.fileHash]["MovieName"], "and", "&"), results);
                    std::string _title(results[0][0] + results[0][1]);

                    if (_stricmp(title.c_str(), _title.c_str()) == 0 /*&& (pSubtitlesInfo.year == -1 || (pSubtitlesInfo.year != -1 && pSubtitlesInfo.year == atoi(_result["data"][pSubtitlesInfo.fileHash]["MovieYear"])))*/) {
                        args[1]["baseinfo"]["idmovieimdb"] = _result["data"][pSubtitlesInfo.fileHash]["MovieImdbID"]; //imdbid
                    }
                }
            }

            if (!args[1]["baseinfo"]["idmovieimdb"].valid()) {
                XmlRpcValue _args, _result;
                _args[0] = token;
                _args[1][0] = pSubtitlesInfo.fileHash;
                if (!xmlrpc->execute("CheckMovieHash2", _args, _result)) { return SR_FAILED; }

                if (_result["data"].getType() == XmlRpcValue::Type::TypeArray) {
                    int nCount = _result["data"][pSubtitlesInfo.fileHash].size();
                    for (int i = 0; i < nCount; ++i) {
                        regexResults results;
                        stringMatch("\"(.+)\" .+|(.+)", StringReplace((const char*)_result["data"][pSubtitlesInfo.fileHash][i]["MovieName"], "and", "&"), results);
                        std::string _title(results[0][0] + results[0][1]);

                        if (_stricmp(title.c_str(), _title.c_str()) == 0 /*&& (pSubtitlesInfo.year == -1 || (pSubtitlesInfo.year != -1 && pSubtitlesInfo.year == atoi(_result["data"][pSubtitlesInfo.fileHash][i]["MovieYear"])))*/) {
                            args[1]["baseinfo"]["idmovieimdb"] = _result["data"][pSubtitlesInfo.fileHash][i]["MovieImdbID"]; //imdbid
                            break;
                        }
                    }
                }
            }

            if (!args[1]["baseinfo"]["idmovieimdb"].valid()) {
                XmlRpcValue _args, _result;
                _args[0] = token;
                _args[1] = title;
                if (!xmlrpc->execute("SearchMoviesOnIMDB", _args, _result)) { return SR_FAILED; }
                if (_result["data"].getType() == XmlRpcValue::Type::TypeArray) {
                    int nCount = _result["data"].size();
                    for (int i = 0; i < nCount; ++i) {
                        regexResults results;
                        stringMatch("(.+) [(](\\d{4})[)]", StringReplace((const char*)_result["data"][i]["title"], "and", "&"), results);
                        if (results.size() == 1) {
                            std::string _title(results[0][0]);

                            if (_stricmp(title.c_str(), _title.c_str()) == 0 /*&& (pSubtitlesInfo.year == -1 || (pSubtitlesInfo.year != -1 && pSubtitlesInfo.year == atoi(results[0][1].c_str())))*/) {
                                args[1]["baseinfo"]["idmovieimdb"] = _result["data"][i]["id"]; //imdbid
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (args[1]["baseinfo"]["idmovieimdb"].valid()) {
            XmlRpcValue _args, _result;
            _args[0] = token;
            _args[1][0]["moviehash"] = pSubtitlesInfo.fileHash;
            _args[1][0]["moviebytesize"] = (int)pSubtitlesInfo.fileSize;
            _args[1][0]["imdbid"] = args[1]["baseinfo"]["idmovieimdb"];
            //_args[1][0]["movietimems"];
            //_args[1][0]["moviefps"];
            _args[1][0]["moviefilename"] = pSubtitlesInfo.fileName + "." + pSubtitlesInfo.fileExtension;
            if (!xmlrpc->execute("InsertMovieHash", _args, _result)) { return SR_FAILED; }
            // REsult value is irrelevant
            _result["data"]["accepted_moviehashes"];


            //args[1]["baseinfo"]["moviereleasename"];
            //args[1]["baseinfo"]["movieaka"];
            //args[1]["baseinfo"]["sublanguageid"];
            //args[1]["baseinfo"]["subauthorcomment"];
            if (pSubtitlesInfo.hearingImpaired != -1) {
                args[1]["baseinfo"]["hearingimpaired"] = pSubtitlesInfo.hearingImpaired;
            }
            //args[1]["baseinfo"]["highdefinition"];
            //args[1]["baseinfo"]["automatictranslation"];

            args[1]["cd1"]["subcontent"] = Base64::encode(StringGzipCompress(pSubtitlesInfo.fileContents));

            if (!xmlrpc->execute("UploadSubtitles", args, result)) { return SR_FAILED; }
            //#ifdef _DEBUG
            ShellExecute((HWND)AfxGetMyApp()->GetMainWnd(), _T("open"), UTF8To16(result["data"]), nullptr, nullptr, SW_SHOWDEFAULT);
            //#endif
            return SR_SUCCEEDED;
        }
    }
    return SR_FAILED;
}

std::string OpenSubtitles::Languages()
{
    static std::string data;
    if (data.empty() && CheckInternetConnection()) {
        XmlRpcValue args, result;
        args = "en";
        if (!xmlrpc->execute("GetSubLanguages", args, result)) { return data; }

        if (result["data"].getType() != XmlRpcValue::Type::TypeArray) { return data; }

        int count = result["data"].size();
        for (int i = 0; i < count; ++i) {
            if (i != 0) { data.append(","); }
            data.append(result["data"][i]["SubLanguageID"]);
        }
    }
    return data;
}

/******************************************************************************
** SubDB
******************************************************************************/

SRESULT SubDB::Hash(SubtitlesInfo& pFileInfo)
{
    std::vector<BYTE> buffer(2 * PROBE_SIZE);
    if (pFileInfo.pAsyncReader) {
        UINT64 position = 0;
        pFileInfo.pAsyncReader->SyncRead(position, PROBE_SIZE, (BYTE*)&buffer[0]);
        position = std::max((UINT64)0, (UINT64)(pFileInfo.fileSize - PROBE_SIZE));
        pFileInfo.pAsyncReader->SyncRead(position, PROBE_SIZE, (BYTE*)&buffer[PROBE_SIZE]);
    } else {
        CFile file;
        CFileException fileException;
        if (file.Open(CString(pFileInfo.filePath.c_str()),
                      CFile::modeRead | CFile::osSequentialScan | CFile::shareDenyNone | CFile::typeBinary,
                      &fileException)) {
            file.Read(&buffer[0], PROBE_SIZE);
            file.Seek(std::max((UINT64)0, (UINT64)(pFileInfo.fileSize - PROBE_SIZE)), CFile::begin);
            file.Read(&buffer[PROBE_SIZE], PROBE_SIZE);
        }
    }
    pFileInfo.fileHash = StringToHash(std::string((char*)&buffer[0], buffer.size()), CALG_MD5);
    return SR_SUCCEEDED;
}

SRESULT SubDB::Search(const SubtitlesInfo& pFileInfo)
{
    SRESULT searchResult = SR_UNDEFINED;
    std::string data;
    searchResult = DownloadInternal(StringFormat("http://api.thesubdb.com/?action=search&hash=%s", pFileInfo.fileHash.c_str()), "", data);

    if (!data.empty()) {
        stringArray result(StringTokenize(data, ","));
        for (const auto& iter : result) {
            CheckAbortAndReturn();
            if (CheckLanguage(iter)) {
                SubtitlesInfo pSubtitlesInfo;
                pSubtitlesInfo.id = pFileInfo.fileHash;
                pSubtitlesInfo.fileExtension = "srt";
                pSubtitlesInfo.fileName = pFileInfo.fileName + GUESSED_NAME_POSTFIX;
                pSubtitlesInfo.languageCode = iter;
                pSubtitlesInfo.languageName = UTF16To8(ISO639XToLanguage(iter.c_str()));
                pSubtitlesInfo.discNumber = 1;
                pSubtitlesInfo.discCount = 1;
                pSubtitlesInfo.title = pFileInfo.title;
                Set(pSubtitlesInfo);
            }
        }
    }

    return searchResult;
}

SRESULT SubDB::Download(SubtitlesInfo& pSubtitlesInfo)
{
    return DownloadInternal(StringFormat("http://api.thesubdb.com/?action=download&hash=%s&language=%s", pSubtitlesInfo.id.c_str(), pSubtitlesInfo.languageCode.c_str()), "", pSubtitlesInfo.fileContents);
}

SRESULT SubDB::Upload(const SubtitlesInfo& pSubtitlesInfo)
{
#define MULTIPART_BOUNDARY "xYzZY"
    std::string url(StringFormat("http://api.thesubdb.com/?action=upload&hash=%s", pSubtitlesInfo.fileHash.c_str()));
    stringMap headers({
        { "User-Agent", UserAgent() },
        { "Content-Type", "multipart/form-data; boundary=" MULTIPART_BOUNDARY },
    });

    std::string content, data;
    content += StringFormat("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", MULTIPART_BOUNDARY, "hash", pSubtitlesInfo.fileHash.c_str());
    content += StringFormat("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s.%s\"\r\nContent-Type: application/octet-stream\r\nContent-Transfer-Encoding: binary\r\n\r\n",
                            MULTIPART_BOUNDARY, "file", pSubtitlesInfo.fileHash.c_str(), "srt");
    content += pSubtitlesInfo.fileContents;
    content += StringFormat("\r\n--%s--\r\n", MULTIPART_BOUNDARY);

    CheckAbortAndReturn();

    DWORD dwStatusCode = NULL;
    StringUpload(url, headers, content, data, FALSE, &dwStatusCode);

    switch (dwStatusCode) {
        case 201:
            return SR_SUCCEEDED;  //'Uploaded':   (HTTP/1.1 201 Created): If everything was OK, the HTTP status code 201 will be returned.
        case 403:
            return SR_EXISTS;     //'Duplicated': (HTTP/1.1 403 Forbidden): If the subtitle file already exists in our database, the HTTP status code 403 will be returned.
        case 400:
            return SR_FAILED;     //'Malformed':  (HTTP/1.1 400 Bad Request): If the request was malformed, the HTTP status code 400 will be returned.
        case 415:
            return SR_FAILED;     //'Invalid':    (HTTP/1.1 415 Unsupported Media Type): If the subtitle file is not supported by our database, the HTTP status code 415 will be returned.
        default:
            return SR_UNDEFINED;
    }
}

std::string SubDB::Languages()
{
    static std::string result;
    if (result.empty() && CheckInternetConnection()) {
        DownloadInternal("http://api.thesubdb.com/?action=languages", "", result);
    }
    return result;
}

/******************************************************************************
** podnapisi
******************************************************************************/

SRESULT podnapisi::Login(const std::string& sUserName, const std::string& sPassword)
{
    //TODO: implement
    return SR_UNDEFINED;
}

/*
UPDATED
http://www.podnapisi.net/forum/viewtopic.php?f=62&t=26164#p212652
RESULTS ------------------------------------------------
"/sXML/1/"  //Reply in XML format
"/page//"   //Return nth page of results
SEARCH -------------------------------------------------
"/sT/1/"    //Type: -1=all, 0=movies, 1=series, don't specify for auto detection
"/sAKA/1/"  //Include movie title aliases
"/sM//"     //Movie id from www.omdb.si
"/sK//"     //Title url encoded text
"/sY//"     //Year number
"/sTS//"    //Season number
"/sTE//"    //Episode number
"/sR//"     //Release name url encoded text
"/sJ/0/"    //Languages (old integer IDs), comma delimited, 0=all
"/sL/en/"   //Languages in ISO ISO codes (exception are sr-latn and pt-br), comma delimited
"/sEH//"    //Exact hash match (OSH)
"/sMH//"    //Movie hash (OSH)
SEARCH ADDITIONAL --------------------------------------
"/sFT/0/"   //Subtitles Format: 0=all, 1=MicroDVD, 2=SAMI, 3=SSA, 4=SubRip, 5=SubViewer 2.0, 6=SubViewer, 7=MPSub, 8=Advanced SSA, 9=DVDSubtitle, 10=TMPlayer, 11=MPlayer2
"/sA/0/"    //Search subtitles by user id, 0=all
"/sI//"     //Search subtitles by subtitle id
SORTING ------------------------------------------------
"/sS//"     //Sorting field: movie, year, fps, language, downloads, cds, username, time, rating
"/sO//"     //Soring order: asc, desc
FILTERS ------------------------------------------------
"/sOE/1/"   //Subtitles for extended edition only
"/sOD/1/"   //Subtitles suitable for DVD only
"/sOH/1/"   //Subtitles for high-definition video only
"/sOI/1/"   //Subtitles for hearing impaired only
"/sOT/1/"   //Technically correct only
"/sOL/1/"   //Grammatically correct only
"/sOA/1/"   //Author subtitles only
"/sOCS/1/"  //Only subtitles for a complete season
UNKNOWN ------------------------------------------------
"/sH//"     //Search subtitles by video file hash ??? (not working for me)
*/

SRESULT podnapisi::Search(const SubtitlesInfo& pFileInfo)
{
    SRESULT searchResult = SR_UNDEFINED;
    int page = 1, pages = 1, results = 0;
    do {
        CheckAbortAndReturn();

        std::string languages(GetLanguagesString());

        std::string search(pFileInfo.title);
        if (!pFileInfo.country.empty()) { search += " " + pFileInfo.country; }
        search = std::regex_replace(search, std::regex(" and | *[!?&':] *", RegexFlags), " ");

        std::string url("http://www.podnapisi.net/ppodnapisi/search");
        url += "?sXML=1";
        url += "&sAKA=1";
        url += (!search.empty() ? "&sK=" + UrlEncode(search.c_str()) : "");
        url += (pFileInfo.year != -1 ? "&sY=" + std::to_string(pFileInfo.year) : "");
        url += (pFileInfo.seasonNumber != -1 ? "&sTS=" + std::to_string(pFileInfo.seasonNumber) : "");
        url += (pFileInfo.episodeNumber != -1 ? "&sTE=" + std::to_string(pFileInfo.episodeNumber) : "");
        url += "&sMH=" + pFileInfo.fileHash;
        //url += "&sR=" + UrlEncode(pFileInfo.fileName.c_str());
        url += (!languages.empty() ? "&sJ=" + languages : "");
        url += "&page=" + std::to_string(page);
        TRACE(_T("%S::Search => %S\n"), Name().c_str(), url.c_str());

        std::string data;
        searchResult = DownloadInternal(url, "", data);

        using namespace tinyxml2;

        tinyxml2::XMLDocument dxml;
        if (dxml.Parse(data.c_str()) == XML_SUCCESS) {

            auto GetChildElementText = [&](XMLElement * pElement, const char* value) -> std::string {
                std::string str;
                XMLElement* pChildElement = pElement->FirstChildElement(value);
                if (pChildElement != nullptr)
                {
                    auto pText = pChildElement->GetText();
                    if (pText != nullptr) { str = pText; }
                }
                return str;
            };

            XMLElement* pRootElmt = dxml.FirstChildElement("results");
            if (pRootElmt) {
                XMLElement* pPaginationElmt = pRootElmt->FirstChildElement("pagination");
                if (pPaginationElmt) {
                    page = atoi(GetChildElementText(pPaginationElmt, "current").c_str());
                    pages = atoi(GetChildElementText(pPaginationElmt, "count").c_str());
                    results = atoi(GetChildElementText(pPaginationElmt, "results").c_str());
                }
                // 30 results per page
                if (page > 1) { return SR_TOOMANY; }

                if (results > 0) {
                    XMLElement* pSubtitleElmt = pRootElmt->FirstChildElement("subtitle");

                    while (pSubtitleElmt) {
                        CheckAbortAndReturn();

                        SubtitlesInfo pSubtitlesInfo;

                        pSubtitlesInfo.id = GetChildElementText(pSubtitleElmt, "pid");
                        pSubtitlesInfo.title = HtmlSpecialCharsDecode(GetChildElementText(pSubtitleElmt, "title").c_str());

                        std::string year = GetChildElementText(pSubtitleElmt, "year");
                        pSubtitlesInfo.year = year.empty() ? -1 : atoi(year.c_str());

                        pSubtitlesInfo.url = GetChildElementText(pSubtitleElmt, "url");
                        std::string format = GetChildElementText(pSubtitleElmt, "format");
                        pSubtitlesInfo.fileExtension = (format == "SubRip" || format == "N/A") ? "srt" : format;

                        pSubtitlesInfo.releaseName = GetChildElementText(pSubtitleElmt, "release");
                        pSubtitlesInfo.languageCode = podnapisi_languages[atoi(GetChildElementText(pSubtitleElmt, "languageId").c_str())].code;
                        pSubtitlesInfo.languageName = GetChildElementText(pSubtitleElmt, "languageName");
                        pSubtitlesInfo.seasonNumber = atoi(GetChildElementText(pSubtitleElmt, "tvSeason").c_str());
                        pSubtitlesInfo.episodeNumber = atoi(GetChildElementText(pSubtitleElmt, "tvEpisode").c_str());
                        pSubtitlesInfo.discCount = atoi(GetChildElementText(pSubtitleElmt, "cds").c_str());
                        pSubtitlesInfo.discNumber = pSubtitlesInfo.discCount;

                        std::string flags = GetChildElementText(pSubtitleElmt, "flags");
                        pSubtitlesInfo.hearingImpaired = (flags.find("n") != std::string::npos) ? TRUE : FALSE;
                        pSubtitlesInfo.corrected = (flags.find("r") != std::string::npos) ? -1 : 0;
                        pSubtitlesInfo.downloadCount = atoi(GetChildElementText(pSubtitleElmt, "downloads").c_str());
                        pSubtitlesInfo.imdbid = GetChildElementText(pSubtitleElmt, "movieId");
                        pSubtitlesInfo.frameRate = atof(GetChildElementText(pSubtitleElmt, "fps").c_str());

                        stringArray fileNames(StringTokenize(pSubtitlesInfo.releaseName, " "));
                        if (fileNames.empty()) {
                            std::string str = pSubtitlesInfo.title;
                            if (!year.empty()) { str += " " + year; }
                            if (pSubtitlesInfo.seasonNumber > 0) { str += StringFormat(" S%02d", pSubtitlesInfo.seasonNumber); }
                            if (pSubtitlesInfo.episodeNumber > 0) { str += StringFormat("%sE%02d", (pSubtitlesInfo.seasonNumber > 0) ? "" : " ", pSubtitlesInfo.episodeNumber); }
                            str += GUESSED_NAME_POSTFIX;
                            fileNames.push_back(str);
                        }
                        pSubtitlesInfo.fileName = fileNames[0] + "." + pSubtitlesInfo.fileExtension;
                        for (const auto& fileName : fileNames) {
                            if (fileName == pFileInfo.fileName) {
                                pSubtitlesInfo.fileName = fileName + "." + pSubtitlesInfo.fileExtension;
                            }
                        }
                        Set(pSubtitlesInfo);
                        pSubtitleElmt = pSubtitleElmt->NextSiblingElement();
                    }
                }
            }
        }
    } while (page++ < pages);

    return searchResult;
}

SRESULT podnapisi::Hash(SubtitlesInfo& pFileInfo)
{
    pFileInfo.fileHash = StringFormat("%016I64x", GenerateOSHash(pFileInfo));
    TRACE(_T("%S::Hash = %S\n"), Name().c_str(), pFileInfo.fileHash.c_str());
    return SR_SUCCEEDED;
}

SRESULT podnapisi::Download(SubtitlesInfo& pSubtitlesInfo)
{
    std::string url = StringFormat("http://www.podnapisi.net/subtitles/%s/download", pSubtitlesInfo.id);
    TRACE(_T("%S::Download => %S\n"), Name().c_str(), url.c_str());

    return DownloadInternal(url, "", pSubtitlesInfo.fileContents);
}

std::string podnapisi::Languages()
{
    static std::string result;
    if (result.empty()) {
        for (const auto& iter : podnapisi_languages) {
            if (strlen(iter.code) && result.find(iter.code) == std::string::npos) {
                result += (result.empty() ? "" : ",");
                result += iter.code;
            }
        }
    }
    return result;
}

std::string podnapisi::GetLanguagesString() const
{
    std::string result;
    std::string languages(LanguagesISO6391());
    if (!languages.empty()) {
        for (const auto& iter : podnapisi_languages) {
            if (strlen(iter.code) && languages.find(iter.code) != std::string::npos) {
                result += (result.empty() ? "" : ",") + std::to_string(&iter - &podnapisi_languages[0]);
            }
        }
    }
    return result;
}

/******************************************************************************
** titlovi
******************************************************************************/

/*
 x-dev_api_id=
 uiculture=hr,rs,si,ba,en,mk
 language=hr,rs,sr,si,ba,en,mk
 keyword=
 year=
 mt=numeric value representing type of subtitle (Movie / TV show / documentary 1, 2, 3)
 season=numeric value representing season
 episode=numeric value representing season episode
 forcefilename=true (default is false) return direct download link
*/

SRESULT titlovi::Search(const SubtitlesInfo& pFileInfo)
{
    SRESULT searchResult = SR_UNDEFINED;

    std::string languages = GetLanguagesString();
    if (!LanguagesISO6391().empty() && languages.empty()) {
        return searchResult;
    }

    std::string KEY = "WC1ERVYtREVTS1RPUF9maWUyYS1hMVJzYS1hSHc0UA==";
    std::string url(StringFormat("http://api.titlovi.com/xml_get_api.ashx?x-dev_api_id=%s&uiculture=en&forcefilename=true", Base64::decode(KEY).c_str()));
    url += "&mt=" + (pFileInfo.seasonNumber != -1 ? std::to_string(2) : std::to_string(1));
    url += "&keyword=" + UrlEncode(pFileInfo.title.c_str());
    url += (pFileInfo.seasonNumber != -1 ? "&season=" + std::to_string(pFileInfo.seasonNumber) : "");
    url += (pFileInfo.episodeNumber != -1 ? "&episode=" + std::to_string(pFileInfo.episodeNumber) : "");
    url += (pFileInfo.year != -1 ? "&year=" + std::to_string(pFileInfo.year) : "");
    url += (!languages.empty() ? "&language=" + languages : "");

    std::string data;
    searchResult = DownloadInternal(url, "", data);

    tinyxml2::XMLDocument dxml;
    if (dxml.Parse(data.c_str()) == tinyxml2::XMLError::XML_SUCCESS) {

        auto GetChildElementText = [&](tinyxml2::XMLElement * pElement, const char* value) -> std::string {
            std::string str;
            auto pChildElement = pElement->FirstChildElement(value);
            if (pChildElement != nullptr)
            {
                auto pText = pChildElement->GetText();
                if (pText != nullptr) { str = pText; }
            }
            return str;
        };

        auto pRootElmt = dxml.FirstChildElement("subtitles");
        if (pRootElmt) {
            std::string name = pRootElmt->Name();
            std::string strAttr = pRootElmt->Attribute("resultsCount");
            int num = pRootElmt->IntAttribute("resultsCount");
            if (num > 0/* && num < 50*/) {
                auto pSubtitleElmt = pRootElmt->FirstChildElement();

                while (pSubtitleElmt) {
                    SubtitlesInfo pSubtitlesInfo;

                    pSubtitlesInfo.title = GetChildElementText(pSubtitleElmt, "title");
                    pSubtitlesInfo.languageCode = GetChildElementText(pSubtitleElmt, "language");
                    for (const auto& language : titlovi_languages) { if (pSubtitlesInfo.languageCode == language.code) { pSubtitlesInfo.languageCode = language.name; } }
                    pSubtitlesInfo.languageName = UTF16To8(ISO639XToLanguage(pSubtitlesInfo.languageCode.c_str()));
                    pSubtitlesInfo.releaseName = GetChildElementText(pSubtitleElmt, "release");
                    pSubtitlesInfo.imdbid = GetChildElementText(pSubtitleElmt, "imdbId");
                    pSubtitlesInfo.frameRate = atof(GetChildElementText(pSubtitleElmt, "fps").c_str());
                    pSubtitlesInfo.year = atoi(GetChildElementText(pSubtitleElmt, "year").c_str());
                    pSubtitlesInfo.discNumber = atoi(GetChildElementText(pSubtitleElmt, "cd").c_str());
                    pSubtitlesInfo.discCount = pSubtitlesInfo.discNumber;
                    pSubtitlesInfo.downloadCount = atoi(GetChildElementText(pSubtitleElmt, "downloads").c_str());

                    auto pSubtitleChildElmt = pSubtitleElmt->FirstChildElement("urls");
                    if (pSubtitleChildElmt) {
                        auto pURLElement = pSubtitleChildElmt->FirstChildElement("url");
                        while (pURLElement) {
                            if (pURLElement->Attribute("what", "download")) {
                                pSubtitlesInfo.url = pURLElement->GetText();
                            }
                            if (pURLElement->Attribute("what", "direct")) {
                                pSubtitlesInfo.id = pURLElement->GetText();
                            }
                            pURLElement = pURLElement->NextSiblingElement();
                        }
                    }

                    if ((pSubtitleChildElmt = pSubtitleElmt->FirstChildElement("TVShow")) != nullptr) {
                        pSubtitlesInfo.seasonNumber = atoi(GetChildElementText(pSubtitleChildElmt, "season").c_str());
                        pSubtitlesInfo.episodeNumber = atoi(GetChildElementText(pSubtitleChildElmt, "episode").c_str());
                    }
                    pSubtitlesInfo.fileName = pSubtitlesInfo.title + " " + std::to_string(pSubtitlesInfo.year);
                    if (pSubtitlesInfo.seasonNumber > 0) { pSubtitlesInfo.fileName += StringFormat(" S%02d", pSubtitlesInfo.seasonNumber); }
                    if (pSubtitlesInfo.episodeNumber > 0) { pSubtitlesInfo.fileName += StringFormat("%sE%02d", (pSubtitlesInfo.seasonNumber > 0) ? "" : " ", pSubtitlesInfo.episodeNumber); }
                    pSubtitlesInfo.fileName += " " + pSubtitlesInfo.releaseName;
                    pSubtitlesInfo.fileName += GUESSED_NAME_POSTFIX;

                    Set(pSubtitlesInfo);
                    pSubtitleElmt = pSubtitleElmt->NextSiblingElement();
                }
            }
        }
    }
    return searchResult;
}

SRESULT titlovi::Download(SubtitlesInfo& pSubtitlesInfo)
{
    return DownloadInternal(pSubtitlesInfo.id.c_str(), "", pSubtitlesInfo.fileContents);
}

std::string titlovi::Languages()
{
    static std::string result;
    if (result.empty()) {
        for (const auto& iter : titlovi_languages) {
            if (strlen(iter.name) && result.find(iter.name) == std::string::npos) {
                result += (result.empty() ? "" : ",");
                result += iter.name;
            }
        }
    }
    return result; // "hr,sr,sl,bs,en,mk";
}

std::string titlovi::GetLanguagesString()
{
    std::string result;
    std::string languages(LanguagesISO6391());
    if (!languages.empty()) {
        for (const auto& iter : titlovi_languages) {
            if (strlen(iter.name) && languages.find(iter.name) != std::string::npos) {
                result += (result.empty() ? "" : ",") + std::string(iter.code);
            }
        }
    }
    return result;
}

/******************************************************************************
** ysubs
******************************************************************************/

SRESULT ysubs::Search(const SubtitlesInfo& pFileInfo)
{
    SRESULT searchResult = SR_UNDEFINED;
    using namespace rapidjson;

    if (pFileInfo.year && pFileInfo.seasonNumber == -1 && pFileInfo.episodeNumber == -1) {
        std::string urlApi(StringFormat("https://yts.ag/api/v2/list_movies.json?query_term=%s", UrlEncode(pFileInfo.title.c_str())));
        TRACE(_T("%S::Search => %S\n"), Name().c_str(), urlApi.c_str());

        std::string data;
        searchResult = DownloadInternal(urlApi, "", data);

        Document d;
        if (d.ParseInsitu(&data[0]).HasParseError()) {
            return SR_FAILED;
        }

        auto root = d.FindMember("data");
        if (root != d.MemberEnd()) {
            auto iter = root->value.FindMember("movies");
            if ((iter != root->value.MemberEnd()) && (iter->value.IsArray())) {
                std::set<std::string> imdb_ids;
                for (auto elem = iter->value.Begin(); elem != iter->value.End(); ++elem) {
                    std::string imdb = elem->FindMember("imdb_code")->value.GetString();
                    if (imdb_ids.find(imdb) == imdb_ids.end()) {
                        imdb_ids.insert(imdb);

                        std::string urlSubs(StringFormat("http://api.ysubs.com/subs/%s", imdb.c_str()));
                        TRACE(_T("%S::Search => %S\n"), Name().c_str(), urlSubs.c_str());

                        std::string data1;
                        searchResult = DownloadInternal(urlSubs, "", data1);
                        Document d1;
                        if (d1.ParseInsitu(&data1[0]).HasParseError()) {
                            return SR_FAILED;
                        }

                        auto iter1 = d1.FindMember("subs");
                        if (iter1 != d1.MemberEnd()) {
                            iter1 = iter1->value.FindMember(imdb.c_str());
                            if (iter1 != d1.MemberEnd()) {
                                for (auto elem1 = iter1->value.MemberBegin(); elem1 != iter1->value.MemberEnd(); ++elem1) {
                                    std::string lang = elem1->name.GetString();
                                    std::string lang_code;
                                    for (const auto& language : ysubs_languages) { if (lang == language.name) { lang_code = language.code; } }
                                    if (CheckLanguage(lang_code)) {
                                        for (auto elem2 = elem1->value.Begin(); elem2 != elem1->value.End(); ++elem2) {
                                            SubtitlesInfo pSubtitlesInfo;

                                            pSubtitlesInfo.title = elem->FindMember("title")->value.GetString();
                                            pSubtitlesInfo.languageCode = lang_code;
                                            pSubtitlesInfo.languageName = UTF16To8(ISO639XToLanguage(pSubtitlesInfo.languageCode.c_str()));
                                            pSubtitlesInfo.releaseName = "YIFY";
                                            pSubtitlesInfo.imdbid = imdb;
                                            pSubtitlesInfo.year = elem->FindMember("year")->value.GetInt();
                                            pSubtitlesInfo.discNumber = 1;
                                            pSubtitlesInfo.discCount = 1;

                                            pSubtitlesInfo.url = "http://www.yifysubtitles.com/movie-imdb/" + imdb;
                                            std::string str = elem2->FindMember("url")->value.GetString();
                                            pSubtitlesInfo.id = "http://www.yifysubtitles.com" + str;
                                            pSubtitlesInfo.hearingImpaired = elem2->FindMember("hi")->value.GetInt();
                                            pSubtitlesInfo.corrected = elem2->FindMember("rating")->value.GetInt();

                                            pSubtitlesInfo.fileName = pFileInfo.fileName;
                                            pSubtitlesInfo.fileName += GUESSED_NAME_POSTFIX;

                                            Set(pSubtitlesInfo);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return searchResult;
}

SRESULT ysubs::Download(SubtitlesInfo& pSubtitlesInfo)
{
    TRACE(_T("%S::Download => %S\n"), Name().c_str(), pSubtitlesInfo.id.c_str());
    return DownloadInternal(pSubtitlesInfo.id.c_str(), "", pSubtitlesInfo.fileContents);
}

std::string ysubs::Languages()
{
    static std::string result;
    if (result.empty()) {
        for (const auto& iter : ysubs_languages) {
            if (strlen(iter.code) && result.find(iter.code) == std::string::npos) {
                result += (result.empty() ? "" : ",");
                result += iter.code;
            }
        }
    }
    return result;
}
