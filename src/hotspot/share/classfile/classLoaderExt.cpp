/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/classFileParser.hpp"
#include "classfile/classFileStream.hpp"
#include "classfile/classListParser.hpp"
#include "classfile/classLoader.inline.hpp"
#include "classfile/classLoaderExt.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/klassFactory.hpp"
#include "classfile/sharedClassUtil.hpp"
#include "classfile/sharedPathsMiscInfo.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmSymbols.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/filemap.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/arguments.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/os.hpp"
#include "services/threadService.hpp"
#include "utilities/stringUtils.hpp"

jshort ClassLoaderExt::_app_paths_start_index = ClassLoaderExt::max_classpath_index;
bool ClassLoaderExt::_has_app_classes = false;
bool ClassLoaderExt::_has_platform_classes = false;

void ClassLoaderExt::setup_app_search_path() {
  assert(DumpSharedSpaces, "this function is only used with -Xshare:dump and -XX:+UseAppCDS");
  _app_paths_start_index = ClassLoader::num_boot_classpath_entries();
  char* app_class_path = os::strdup(Arguments::get_appclasspath());

  if (strcmp(app_class_path, ".") == 0) {
    // This doesn't make any sense, even for AppCDS, so let's skip it. We
    // don't want to throw an error here because -cp "." is usually assigned
    // by the launcher when classpath is not specified.
    trace_class_path("app loader class path (skipped)=", app_class_path);
  } else {
    trace_class_path("app loader class path=", app_class_path);
    shared_paths_misc_info()->add_app_classpath(app_class_path);
    ClassLoader::setup_app_search_path(app_class_path);
  }
}

char* ClassLoaderExt::read_manifest(ClassPathEntry* entry, jint *manifest_size, bool clean_text, TRAPS) {
  const char* name = "META-INF/MANIFEST.MF";
  char* manifest;
  jint size;

  assert(entry->is_jar_file(), "must be");
  manifest = (char*) ((ClassPathZipEntry*)entry )->open_entry(name, &size, true, CHECK_NULL);

  if (manifest == NULL) { // No Manifest
    *manifest_size = 0;
    return NULL;
  }


  if (clean_text) {
    // See http://docs.oracle.com/javase/6/docs/technotes/guides/jar/jar.html#JAR%20Manifest
    // (1): replace all CR/LF and CR with LF
    StringUtils::replace_no_expand(manifest, "\r\n", "\n");

    // (2) remove all new-line continuation (remove all "\n " substrings)
    StringUtils::replace_no_expand(manifest, "\n ", "");
  }

  *manifest_size = (jint)strlen(manifest);
  return manifest;
}

char* ClassLoaderExt::get_class_path_attr(const char* jar_path, char* manifest, jint manifest_size) {
  const char* tag = "Class-Path: ";
  const int tag_len = (int)strlen(tag);
  char* found = NULL;
  char* line_start = manifest;
  char* end = manifest + manifest_size;

  assert(*end == 0, "must be nul-terminated");

  while (line_start < end) {
    char* line_end = strchr(line_start, '\n');
    if (line_end == NULL) {
      // JAR spec require the manifest file to be terminated by a new line.
      break;
    }
    if (strncmp(tag, line_start, tag_len) == 0) {
      if (found != NULL) {
        // Same behavior as jdk/src/share/classes/java/util/jar/Attributes.java
        // If duplicated entries are found, the last one is used.
        tty->print_cr("Warning: Duplicate name in Manifest: %s.\n"
                      "Ensure that the manifest does not have duplicate entries, and\n"
                      "that blank lines separate individual sections in both your\n"
                      "manifest and in the META-INF/MANIFEST.MF entry in the jar file:\n%s\n", tag, jar_path);
      }
      found = line_start + tag_len;
      assert(found <= line_end, "sanity");
      *line_end = '\0';
    }
    line_start = line_end + 1;
  }
  return found;
}

void ClassLoaderExt::process_jar_manifest(ClassPathEntry* entry,
                                          bool check_for_duplicates) {
  Thread* THREAD = Thread::current();
  ResourceMark rm(THREAD);
  jint manifest_size;
  char* manifest = read_manifest(entry, &manifest_size, CHECK);

  if (manifest == NULL) {
    return;
  }

  if (strstr(manifest, "Extension-List:") != NULL) {
    tty->print_cr("-Xshare:dump does not support Extension-List in JAR manifest: %s", entry->name());
    vm_exit(1);
  }

  char* cp_attr = get_class_path_attr(entry->name(), manifest, manifest_size);

  if (cp_attr != NULL && strlen(cp_attr) > 0) {
    trace_class_path("found Class-Path: ", cp_attr);

    char sep = os::file_separator()[0];
    const char* dir_name = entry->name();
    const char* dir_tail = strrchr(dir_name, sep);
    int dir_len;
    if (dir_tail == NULL) {
      dir_len = 0;
    } else {
      dir_len = dir_tail - dir_name + 1;
    }

    // Split the cp_attr by spaces, and add each file
    char* file_start = cp_attr;
    char* end = file_start + strlen(file_start);

    while (file_start < end) {
      char* file_end = strchr(file_start, ' ');
      if (file_end != NULL) {
        *file_end = 0;
        file_end += 1;
      } else {
        file_end = end;
      }

      int name_len = (int)strlen(file_start);
      if (name_len > 0) {
        ResourceMark rm(THREAD);
        char* libname = NEW_RESOURCE_ARRAY(char, dir_len + name_len + 1);
        *libname = 0;
        strncat(libname, dir_name, dir_len);
        strncat(libname, file_start, name_len);
        trace_class_path("library = ", libname);
        ClassLoader::update_class_path_entry_list(libname, true, false);
      }

      file_start = file_end;
    }
  }
}

void ClassLoaderExt::setup_search_paths() {
  if (UseAppCDS) {
    shared_paths_misc_info()->record_app_offset();
    ClassLoaderExt::setup_app_search_path();
  }
}

Thread* ClassLoaderExt::Context::_dump_thread = NULL;

bool ClassLoaderExt::check(ClassLoaderExt::Context *context,
                           const ClassFileStream* stream,
                           const int classpath_index) {
  if (stream != NULL) {
    // Ignore any App classes from signed JAR file during CDS archiving
    // dumping
    if (DumpSharedSpaces &&
        SharedClassUtil::is_classpath_entry_signed(classpath_index) &&
        classpath_index >= _app_paths_start_index) {
      tty->print_cr("Preload Warning: Skipping %s from signed JAR",
                    context->class_name());
      return false;
    }
    if (classpath_index >= _app_paths_start_index) {
      _has_app_classes = true;
      _has_platform_classes = true;
    }
  }

  return true;
}

void ClassLoaderExt::record_result(ClassLoaderExt::Context *context,
                                   Symbol* class_name,
                                   const s2 classpath_index,
                                   InstanceKlass* result,
                                   TRAPS) {
  assert(DumpSharedSpaces, "Sanity");

  // We need to remember where the class comes from during dumping.
  oop loader = result->class_loader();
  s2 classloader_type = ClassLoader::BOOT_LOADER;
  if (SystemDictionary::is_system_class_loader(loader)) {
    classloader_type = ClassLoader::APP_LOADER;
    ClassLoaderExt::set_has_app_classes();
  } else if (SystemDictionary::is_platform_class_loader(loader)) {
    classloader_type = ClassLoader::PLATFORM_LOADER;
    ClassLoaderExt::set_has_platform_classes();
  }
  result->set_shared_classpath_index(classpath_index);
  result->set_class_loader_type(classloader_type);
}

void ClassLoaderExt::finalize_shared_paths_misc_info() {
  if (UseAppCDS) {
    if (!_has_app_classes) {
      shared_paths_misc_info()->pop_app();
    }
  }
}

// Load the class of the given name from the location given by path. The path is specified by
// the "source:" in the class list file (see classListParser.cpp), and can be a directory or
// a JAR file.
InstanceKlass* ClassLoaderExt::load_class(Symbol* name, const char* path, TRAPS) {

  assert(name != NULL, "invariant");
  assert(DumpSharedSpaces && UseAppCDS, "this function is only used with -Xshare:dump and -XX:+UseAppCDS");
  ResourceMark rm(THREAD);
  const char* class_name = name->as_C_string();

  const char* file_name = file_name_for_class_name(class_name,
                                                   name->utf8_length());
  assert(file_name != NULL, "invariant");

  // Lookup stream for parsing .class file
  ClassFileStream* stream = NULL;
  ClassPathEntry* e = find_classpath_entry_from_cache(path, CHECK_NULL);
  if (e == NULL) {
    return NULL;
  }
  {
    PerfClassTraceTime vmtimer(perf_sys_class_lookup_time(),
                               ((JavaThread*) THREAD)->get_thread_stat()->perf_timers_addr(),
                               PerfClassTraceTime::CLASS_LOAD);
    stream = e->open_stream(file_name, CHECK_NULL);
  }

  if (NULL == stream) {
    tty->print_cr("Preload Warning: Cannot find %s", class_name);
    return NULL;
  }

  assert(stream != NULL, "invariant");
  stream->set_verify(true);

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  Handle protection_domain;

  InstanceKlass* result = KlassFactory::create_from_stream(stream,
                                                           name,
                                                           loader_data,
                                                           protection_domain,
                                                           NULL, // host_klass
                                                           NULL, // cp_patches
                                                           THREAD);

  if (HAS_PENDING_EXCEPTION) {
    tty->print_cr("Preload Error: Failed to load %s", class_name);
    return NULL;
  }
  result->set_shared_classpath_index(UNREGISTERED_INDEX);
  SystemDictionaryShared::set_shared_class_misc_info(result, stream);
  return result;
}

struct CachedClassPathEntry {
  const char* _path;
  ClassPathEntry* _entry;
};

static GrowableArray<CachedClassPathEntry>* cached_path_entries = NULL;

ClassPathEntry* ClassLoaderExt::find_classpath_entry_from_cache(const char* path, TRAPS) {
  // This is called from dump time so it's single threaded and there's no need for a lock.
  assert(DumpSharedSpaces && UseAppCDS, "this function is only used with -Xshare:dump and -XX:+UseAppCDS");
  if (cached_path_entries == NULL) {
    cached_path_entries = new (ResourceObj::C_HEAP, mtClass) GrowableArray<CachedClassPathEntry>(20, /*c heap*/ true);
  }
  CachedClassPathEntry ccpe;
  for (int i=0; i<cached_path_entries->length(); i++) {
    ccpe = cached_path_entries->at(i);
    if (strcmp(ccpe._path, path) == 0) {
      if (i != 0) {
        // Put recent entries at the beginning to speed up searches.
        cached_path_entries->remove_at(i);
        cached_path_entries->insert_before(0, ccpe);
      }
      return ccpe._entry;
    }
  }

  struct stat st;
  if (os::stat(path, &st) != 0) {
    // File or directory not found
    return NULL;
  }
  ClassPathEntry* new_entry = NULL;

  new_entry = create_class_path_entry(path, &st, false, false, CHECK_NULL);
  if (new_entry == NULL) {
    return NULL;
  }
  ccpe._path = strdup(path);
  ccpe._entry = new_entry;
  cached_path_entries->insert_before(0, ccpe);
  return new_entry;
}

Klass* ClassLoaderExt::load_one_class(ClassListParser* parser, TRAPS) {
  return parser->load_current_class(THREAD);
}
