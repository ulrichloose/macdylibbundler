/*
The MIT License (MIT)

Copyright (c) 2014 Marianne Gagnon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
 */

#include "DylibBundler.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <map>
#ifdef __linux
#include <linux/limits.h>
#endif
#include "Utils.h"
#include "Settings.h"
#include "Dependency.h"


std::vector<Dependency> deps;
std::map<std::string, std::vector<Dependency> > deps_per_file;
std::map<std::string, bool> deps_collected;
std::set<std::string> rpaths;
std::map<std::string, std::vector<std::string> > rpaths_per_file;

void changeLibPathsOnFile(std::string file_to_fix)
{
    if (deps_collected.find(file_to_fix) == deps_collected.end())
    {
        collectDependencies(file_to_fix);
    }
    std::cout << "\n* Fixing dependencies on " << file_to_fix.c_str() << std::endl;
    
    std::vector<Dependency> deps_in_file = deps_per_file[file_to_fix];
    const int dep_amount = deps_in_file.size();
    for(int n=0; n<dep_amount; n++)
    {
        deps_in_file[n].fixFileThatDependsOnMe(file_to_fix);
    }
}

bool isRpath(const std::string& path)
{
    return path.find("@rpath") == 0 || path.find("@loader_path") == 0;
}

void collectRpaths(const std::string& filename)
{
    if (!fileExists(filename))
    {
        std::cerr << "\n/!\\ WARNING : can't collect rpaths for nonexistent file '" << filename << "'\n";
        return;
    }

    std::string cmd = "otool -l " + filename;
    std::string output = system_get_output(cmd);

    std::vector<std::string> lc_lines;
    tokenize(output, "\n", &lc_lines);

    size_t pos = 0;
    bool read_rpath = false;
    while (pos < lc_lines.size())
    {
        std::string line = lc_lines[pos];
        pos++;

        if (read_rpath)
        {
            size_t start_pos = line.find("path ");
            size_t end_pos = line.find(" (");
            if (start_pos == std::string::npos || end_pos == std::string::npos)
            {
                std::cerr << "\n/!\\ WARNING: Unexpected LC_RPATH format\n";
                continue;
            }
            start_pos += 5;
            std::string rpath = line.substr(start_pos, end_pos - start_pos);
            rpaths.insert(rpath);
            rpaths_per_file[filename].push_back(rpath);
            read_rpath = false;
            continue;
        }

        if (line.find("LC_RPATH") != std::string::npos)
        {
            read_rpath = true;
            pos++;
        }
    }
}

void collectRpathsForFilename(const std::string& filename)
{
    if (rpaths_per_file.find(filename) == rpaths_per_file.end())
    {
        collectRpaths(filename);
    }
}

std::string searchFilenameInRpaths(const std::string& rpath_file)
{
    char buffer[PATH_MAX];
    std::string fullpath;
    std::string suffix = rpath_file.substr(rpath_file.rfind("/")+1);

    for (std::set<std::string>::iterator it = rpaths.begin(); it != rpaths.end(); ++it)
    {
        std::string path = *it + "/" + suffix;
        if (realpath(path.c_str(), buffer))
        {
            fullpath = buffer;
            break;
        }
    }

    if (fullpath.empty())
    {
        std::cerr << "\n/!\\ WARNING : can't get path for '" << rpath_file << "'\n";
        fullpath = getUserInputDirForFile(suffix) + suffix;
        if (realpath(fullpath.c_str(), buffer)) {
            fullpath = buffer;
        }
    }

    return fullpath;
}

void fixRpathsOnFile(const std::string& original_file, const std::string& file_to_fix)
{
    std::vector<std::string> rpaths_to_fix;
    std::map<std::string, std::vector<std::string> >::iterator found = rpaths_per_file.find(original_file);
    if (found != rpaths_per_file.end())
    {
        rpaths_to_fix = found->second;
    }

    for (size_t i=0; i < rpaths_to_fix.size(); ++i)
    {
        std::string command = std::string("install_name_tool -rpath \"") +
                rpaths_to_fix[i] + "\" \"" + Settings::inside_lib_path() + "\" \"" + file_to_fix + "\"";
        if ( systemp(command) != 0)
        {
            std::cerr << "\n\nError : An error occured while trying to fix dependencies of " << file_to_fix << std::endl;
            exit(1);
        }
    }
}

void addDependency(std::string path, std::string filename)
{
    Dependency dep(path);
    
    // we need to check if this library was already added to avoid duplicates
    bool in_deps = false;
    const int dep_amount = deps.size();
    for(int n=0; n<dep_amount; n++)
    {
        if(dep.mergeIfSameAs(deps[n])) in_deps = true;
    }
    
    // check if this library was already added to |deps_per_file[filename]| to avoid duplicates
    std::vector<Dependency> deps_in_file = deps_per_file[filename];
    bool in_deps_per_file = false;
    const int deps_in_file_amount = deps_in_file.size();
    for(int n=0; n<deps_in_file_amount; n++)
    {
        if(dep.mergeIfSameAs(deps_in_file[n])) in_deps_per_file = true;
    }

    if(!Settings::isPrefixBundled(dep.getPrefix())) return;
    
    if(!in_deps) deps.push_back(dep);
    if(!in_deps_per_file) deps_per_file[filename].push_back(dep);
}

/*
 *  Fill vector 'lines' with dependencies of given 'filename'
 */
void collectDependencies(std::string filename, std::vector<std::string>& lines)
{
    // execute "otool -l" on the given file and collect the command's output
    std::cerr << "Open " << filename << std::endl;
    std::string cmd = "otool -l \"" + filename + "\"";
    std::string output = system_get_output(cmd);

    if(output.find("can't open file")!=std::string::npos or output.find("No such file")!=std::string::npos or output.size()<1)
    {
        std::cerr << "Cannot find file " << filename << " to read its dependencies" << std::endl;
        exit(1);
    }
    
    // split output
    std::vector<std::string> raw_lines;
    tokenize(output, "\n", &raw_lines);

    bool searching = false;
    for(const auto& line : raw_lines) {
        if (line.find("cmd LC_LOAD_DYLIB") != std::string::npos)
        {
            if (searching)
            {
                std::cerr << "\n\n/!\\ ERROR: Failed to find name before next cmd" << std::endl;
                exit(1);
            }
            searching = true;
        }
        else if (searching)
        {
            size_t found = line.find("name ");
            if (found != std::string::npos)
            {
                lines.push_back('\t' + line.substr(found+5, std::string::npos));
                searching = false;
            }
        }
    }
}


void collectDependencies(std::string filename)
{
    std::vector<std::string> lines;
    collectDependencies(filename, lines);
       
    //std::cout << "."; fflush(stdout);
    
    const int line_amount = lines.size();
    for(int n=0; n<line_amount; n++)
    {
        //std::cout << "."; fflush(stdout);
        if(lines[n][0] != '\t') continue; // only lines beginning with a tab interest us
        if( lines[n].find(".framework") != std::string::npos ) continue; //Ignore frameworks, we can not handle them

        // trim useless info, keep only library name
        std::string dep_path = lines[n].substr(1, lines[n].rfind(" (") - 1);
        if (isRpath(dep_path))
        {
            collectRpathsForFilename(filename);
        }

        addDependency(dep_path, filename);
    }
    deps_collected[filename] = true;
}
void collectSubDependencies()
{
    // print status to user
    int dep_amount = deps.size();
    
    // recursively collect each dependencie's dependencies
    while(true)
    {
        dep_amount = deps.size();
        for(int n=0; n<dep_amount; n++)
        {
            //std::cout << "."; fflush(stdout);
            std::vector<std::string> lines;
            std::string original_path = deps[n].getOriginalPath();
            if (isRpath(original_path))
            {
                original_path = searchFilenameInRpaths(original_path);
            }
            collectRpathsForFilename(original_path);
            collectDependencies(original_path, lines);
            
            const int line_amount = lines.size();
            for(int n=0; n<line_amount; n++)
            {
                if(lines[n][0] != '\t') continue; // only lines beginning with a tab interest us
                if( lines[n].find(".framework") != std::string::npos ) continue; //Ignore frameworks, we cannot handle them
                
                // trim useless info, keep only library name
                std::string dep_path = lines[n].substr(1, lines[n].rfind(" (") - 1);
                if (isRpath(dep_path))
                {
                    collectRpathsForFilename(searchFilenameInRpaths(dep_path));
                }

                addDependency(dep_path, original_path);
            }//next
        }//next
        
        if(deps.size() == dep_amount) break; // no more dependencies were added on this iteration, stop searching
    }
}

void createDestDir()
{
    std::string dest_folder = Settings::destFolder();
    std::cout << "* Checking output directory " << dest_folder.c_str() << std::endl;
    
    // ----------- check dest folder stuff ----------
    bool dest_exists = fileExists(dest_folder);
    
    if(dest_exists and Settings::canOverwriteDir())
    {
        std::cout << "* Erasing old output directory " << dest_folder.c_str() << std::endl;
        std::string command = std::string("rm -r \"") + dest_folder + "\"";
        if( systemp( command ) != 0)
        {
            std::cerr << "\n\nError : An error occured while attempting to overwrite dest folder." << std::endl;
            exit(1);
        }
        dest_exists = false;
    }
    
    if(!dest_exists)
    {
        
        if(Settings::canCreateDir())
        {
            std::cout << "* Creating output directory " << dest_folder.c_str() << std::endl;
            std::string command = std::string("mkdir -p \"") + dest_folder + "\"";
            if( systemp( command ) != 0)
            {
                std::cerr << "\n\nError : An error occured while creating dest folder." << std::endl;
                exit(1);
            }
        }
        else
        {
            std::cerr << "\n\nError : Dest folder does not exist. Create it or pass the appropriate flag for automatic dest dir creation." << std::endl;
            exit(1);
        }
    }
    
}

void doneWithDeps_go()
{
    std::cout << std::endl;
    const int dep_amount = deps.size();
    // print info to user
    for(int n=0; n<dep_amount; n++)
    {
        deps[n].print();
    }
    std::cout << std::endl;
    
    // copy files if requested by user
    if(Settings::bundleLibs())
    {
        createDestDir();
        
        for(int n=0; n<dep_amount; n++)
        {
            deps[n].copyYourself();
            changeLibPathsOnFile(deps[n].getInstallPath());
            fixRpathsOnFile(deps[n].getOriginalPath(), deps[n].getInstallPath());
        }
    }
    
    const int fileToFixAmount = Settings::fileToFixAmount();
    for(int n=0; n<fileToFixAmount; n++)
    {
        changeLibPathsOnFile(Settings::fileToFix(n));
        fixRpathsOnFile(Settings::fileToFix(n), Settings::fileToFix(n));
    }
}
