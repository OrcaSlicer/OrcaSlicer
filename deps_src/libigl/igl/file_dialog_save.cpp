// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "file_dialog_save.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
 #include <windows.h>
 #include <Commdlg.h>
#endif

IGL_INLINE std::string igl::file_dialog_save()
{
  const int FILE_DIALOG_MAX_BUFFER = 1024;
  char buffer[FILE_DIALOG_MAX_BUFFER];
  buffer[0] = '\0';
  buffer[FILE_DIALOG_MAX_BUFFER - 1] = 'x'; // Initialize last character with a char != '\0'

#ifdef __APPLE__
  // For apple use applescript hack
  // There is currently a bug in Applescript that strips extensions off
  // of chosen existing files in the "choose file name" dialog
  // I'm assuming that will be fixed soon
  FILE * output = popen(
    "osascript -e \""
    "   tell application \\\"System Events\\\"\n"
    "           activate\n"
    "           set existing_file to choose file name\n"
    "   end tell\n"
    "   set existing_file_path to (POSIX path of (existing_file))\n"
    "\" 2>/dev/null | tr -d '\n' ","r");
  if (output)
  {
    auto ret = fgets(buffer, FILE_DIALOG_MAX_BUFFER, output);
    if (ret == NULL || ferror(output))
    {
      // I/O error
      buffer[0] = '\0';
    }
    if (buffer[FILE_DIALOG_MAX_BUFFER - 1] == '\0')
    {
      // File name too long, buffer has been filled, so we return empty string instead
      buffer[0] = '\0';
    }
  }
#elif defined _WIN32

  // Use native windows file dialog box
  // (code contributed by Tino Weinkauf)

  OPENFILENAME ofn;       // common dialog box structure
  char szFile[260];       // buffer for file name

  // Initialize OPENFILENAME
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = NULL;//hwnd;
  ofn.lpstrFile = szFile;
  // Set lpstrFile[0] to '\0' so that GetOpenFileName does not
  // use the contents of szFile to initialize itself.
  ofn.lpstrFile[0] = '\0';
  ofn.nMaxFile = sizeof(szFile);
  ofn.lpstrFilter = "";
  ofn.nFilterIndex = 1;
  ofn.lpstrFileTitle = NULL;
  ofn.nMaxFileTitle = 0;
  ofn.lpstrInitialDir = NULL;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

  // Display the Open dialog box.
  int pos = 0;
  if (GetSaveFileName(&ofn)==TRUE)
  {
    while(ofn.lpstrFile[pos] != '\0')
    {
      buffer[pos] = (char)ofn.lpstrFile[pos];
      pos++;
    }
    buffer[pos] = 0;
  }

#else
  // For every other machine type use zenity
  FILE * output = popen("/usr/bin/zenity --file-selection --save","r");
  if (output)
  {
    auto ret = fgets(buffer, FILE_DIALOG_MAX_BUFFER, output);
    if (ret == NULL || ferror(output))
    {
      // I/O error
      buffer[0] = '\0';
    }
    if (buffer[FILE_DIALOG_MAX_BUFFER - 1] == '\0')
    {
      // File name too long, buffer has been filled, so we return empty string instead
      buffer[0] = '\0';
    }
  }

  // Replace last '\n' by '\0'
  if(strlen(buffer) > 0)
  {
    buffer[strlen(buffer)-1] = '\0';
  }

#endif
  return std::string(buffer);
}
