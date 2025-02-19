/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2015,2016,2017,2018,2019, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 *
 * \brief This file declares functions for mdrun to call to manage the
 * details of doing a restart (ie. reading checkpoints, appending
 * output files).
 *
 * \todo Clean up the error-prone logic here. Add doxygen.
 *
 * \author Berk Hess <hess@kth.se>
 * \author Erik Lindahl <erik@kth.se>
 * \author Mark Abraham <mark.j.abraham@gmail.com>
 *
 * \ingroup module_mdrunutility
 */

#include "gmxpre.h"

#include "handlerestart.h"

#include "config.h"

#include <cstring>

#include <fcntl.h>
#if GMX_NATIVE_WINDOWS
#include <io.h>
#include <sys/locking.h>
#endif

#include <algorithm>
#include <exception>
#include <functional>
#include <tuple>

#include "gromacs/commandline/filenm.h"
#include "gromacs/fileio/checkpoint.h"
#include "gromacs/fileio/gmxfio.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/mdrunutility/multisim.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/mdrunoptions.h"
#include "gromacs/utility/basedefinitions.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/path.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringstream.h"
#include "gromacs/utility/textwriter.h"

namespace gmx
{
namespace
{

/*! \brief Search for \p fnm_cp in fnm and return true iff found
 *
 * \todo This could be implemented sanely with a for loop. */
gmx_bool exist_output_file(const char *fnm_cp, int nfile, const t_filenm fnm[])
{
    int i;

    /* Check if the output file name stored in the checkpoint file
     * is one of the output file names of mdrun.
     */
    i = 0;
    while (i < nfile &&
           !(is_output(&fnm[i]) && strcmp(fnm_cp, fnm[i].filenames[0].c_str()) == 0))
    {
        i++;
    }

    return (i < nfile && gmx_fexist(fnm_cp));
}

/*! \brief Throw when mdrun -cpi fails because previous output files are missing.
 *
 * If we get here, the user requested restarting from a checkpoint file, that checkpoint
 * file was found (so it is not the first part of a new run), but we are still missing
 * some or all checkpoint files. In this case we issue a fatal error since there are
 * so many special cases we cannot keep track of, and better safe than sorry. */
[[noreturn]] void
throwBecauseOfMissingOutputFiles(const char *checkpointFilename,
                                 ArrayRef<const gmx_file_position_t> outputfiles,
                                 int nfile, const t_filenm fnm[],
                                 size_t numFilesMissing)
{
    StringOutputStream stream;
    TextWriter         writer(&stream);
    writer.writeStringFormatted
        ("Some output files listed in the checkpoint file %s are not present or not named "
        "as the output files by the current program:)",
        checkpointFilename);
    auto settings  = writer.wrapperSettings();
    auto oldIndent = settings.indent(), newIndent = 2;

    writer.writeLine("Expected output files that are present:");
    settings.setIndent(newIndent);
    for (const auto &outputfile : outputfiles)
    {
        if (exist_output_file(outputfile.filename, nfile, fnm))
        {
            writer.writeLine(outputfile.filename);
        }
    }
    settings.setIndent(oldIndent);
    writer.ensureEmptyLine();

    writer.writeLine("Expected output files that are not present or named differently:");
    settings.setIndent(newIndent);
    for (const auto &outputfile : outputfiles)
    {
        if (!exist_output_file(outputfile.filename,
                               nfile, fnm))
        {
            writer.writeLine(outputfile.filename);
        }
    }
    settings.setIndent(oldIndent);

    writer.writeLineFormatted(
            R"(To keep your simulation files safe, this simulation will not restart. Either name your
output files exactly the same as the previous simulation part (e.g. with -deffnm), or
make sure all the output files are present (e.g. run from the same directory as the
previous simulation part), or instruct mdrun to write new output files with mdrun -noappend.
In the last case, you will not be able to use appending in future for this simulation.)",
            numFilesMissing, outputfiles.size());
    GMX_THROW(InconsistentInputError(stream.toString()));
}

//! Return a string describing the precision of a build of GROMACS.
const char *precisionToString(bool isDoublePrecision)
{
    return isDoublePrecision ? "double" : "mixed";
}

/*! \brief Choose the starting behaviour
 *
 * This routine cannot print tons of data, since it is called before
 * the log file is opened. */
std::tuple < StartingBehavior,
CheckpointHeaderContents,
std::vector < gmx_file_position_t>>
chooseStartingBehavior(const AppendingBehavior appendingBehavior,
                       const int nfile,
                       t_filenm fnm[])
{
    CheckpointHeaderContents         headerContents;
    std::vector<gmx_file_position_t> outputFiles;
    if (!opt2bSet("-cpi", nfile, fnm))
    {
        // No need to tell the user anything
        return std::make_tuple(StartingBehavior::NewSimulation, headerContents, outputFiles);
    }

    // A -cpi option was provided, do a restart if there is an input checkpoint file available
    const char *checkpointFilename = opt2fn("-cpi", nfile, fnm);
    if (!gmx_fexist(checkpointFilename))
    {
        // This is interpreted as the user intending a new
        // simulation, so that scripts can call "gmx mdrun -cpi"
        // for all simulation parts. Thus, appending cannot occur.
        if (appendingBehavior == AppendingBehavior::Appending)
        {
            GMX_THROW(InconsistentInputError
                          ("Could not do a restart with appending because the checkpoint file "
                          "was not found. Either supply the name of the right checkpoint file "
                          "or do not use -append"));
        }
        // No need to tell the user that mdrun -cpi without a file means a new simulation
        return std::make_tuple(StartingBehavior::NewSimulation, headerContents, outputFiles);
    }

    t_fileio *fp = gmx_fio_open(checkpointFilename, "r");
    if (fp == nullptr)
    {
        GMX_THROW(FileIOError
                      (formatString("Checkpoint file '%s' was found but could not be opened for "
                                    "reading. Check the file permissions.", checkpointFilename)));
    }

    headerContents =
        read_checkpoint_simulation_part_and_filenames(fp, &outputFiles);

    GMX_RELEASE_ASSERT(!outputFiles.empty(),
                       "The checkpoint file or its reading is broken, as no output "
                       "file information is stored in it");
    const char *logFilename = outputFiles[0].filename;
    GMX_RELEASE_ASSERT(Path::extensionMatches(logFilename, ftp2ext(efLOG)),
                       formatString("The checkpoint file or its reading is broken, the first "
                                    "output file '%s' must be a log file with extension '%s'",
                                    logFilename, ftp2ext(efLOG)).c_str());

    if (appendingBehavior != AppendingBehavior::NoAppending)
    {
        // See whether appending can be done.

        size_t numFilesMissing = std::count_if(std::begin(outputFiles), std::end(outputFiles),
                                               [nfile, fnm](const auto &outputFile)
                                               {
                                                   return !exist_output_file(outputFile.filename, nfile, fnm);
                                               });
        if (numFilesMissing != 0)
        {
            // Appending is not possible, because not all previous
            // output files are present. We don't automatically switch
            // to numbered output files either, because that prevents
            // the user from using appending in future. If they want
            // to restart with missing files, they need to use
            // -noappend.
            throwBecauseOfMissingOutputFiles(checkpointFilename, outputFiles, nfile, fnm, numFilesMissing);
        }

        for (const auto &outputFile : outputFiles)
        {
            if (outputFile.offset < 0)
            {
                // Appending of large files is not possible unless
                // mdrun and the filesystem can do a correct job. We
                // don't automatically switch to numbered output files
                // either, because the user can benefit from
                // understanding that their infrastructure is not very
                // suitable for running a simulation producing lots of
                // output.
                auto message =
                    formatString("The original mdrun wrote a file called '%s' which "
                                 "is larger than 2 GB, but that mdrun or the filesystem "
                                 "it ran on (e.g FAT32) did not support such large files. "
                                 "This simulation cannot be restarted with appending. It will "
                                 "be easier for you to use mdrun on a 64-bit filesystem, but "
                                 "if you choose not to, then you must run mdrun with "
                                 "-noappend once your output gets large enough.",
                                 outputFile.filename);
                GMX_THROW(InconsistentInputError(message));
            }
        }

        const char *logFilename = outputFiles[0].filename;
        // If the precision does not match, we cannot continue with
        // appending, and will switch to not appending unless
        // instructed otherwise.
        if (headerContents.file_version >= 13 && headerContents.double_prec != GMX_DOUBLE)
        {
            if (appendingBehavior == AppendingBehavior::Appending)
            {
                GMX_THROW(InconsistentInputError
                              (formatString("Cannot restart with appending because the previous simulation part used "
                                            "%s precision which does not match the %s precision used by this build "
                                            "of GROMACS. Either use matching precision or use mdrun -noappend.",
                                            precisionToString(headerContents.double_prec),
                                            precisionToString(GMX_DOUBLE))));
            }
        }
        // If the previous log filename had a part number, then we
        // cannot continue with appending, and will continue without
        // appending.
        else if (hasSuffixFromNoAppend(logFilename))
        {
            if (appendingBehavior == AppendingBehavior::Appending)
            {
                GMX_THROW(InconsistentInputError
                              ("Cannot restart with appending because the previous simulation "
                              "part did not use appending. Either do not use mdrun -append, or "
                              "provide the correct checkpoint file."));
            }
        }
        else
        {
            // Everything is perfect - we can do an appending restart.
            return std::make_tuple(StartingBehavior::RestartWithAppending, headerContents, outputFiles);
        }

        // No need to tell the user anything because the previous
        // simulation part also didn't append and that can only happen
        // when they ask for it.
    }

    GMX_RELEASE_ASSERT(appendingBehavior != AppendingBehavior::Appending, "Logic error in appending");
    return std::make_tuple(StartingBehavior::RestartWithoutAppending, headerContents, outputFiles);
}

//! Check whether chksum_file output file has a checksum that matches \c outputfile from the checkpoint.
void checkOutputFile(t_fileio                  *fileToCheck,
                     const gmx_file_position_t &outputfile)
{
    /* compute md5 chksum */
    std::array<unsigned char, 16> digest;
    if (outputfile.checksumSize != -1)
    {
        if (gmx_fio_get_file_md5(fileToCheck, outputfile.offset,
                                 &digest) != outputfile.checksumSize) /*at the end of the call the file position is at the end of the file*/
        {
            auto message =
                formatString("Can't read %d bytes of '%s' to compute checksum. The file "
                             "has been replaced or its contents have been modified. Cannot "
                             "do appending because of this condition.",
                             outputfile.checksumSize,
                             outputfile.filename);
            GMX_THROW(InconsistentInputError(message));
        }
    }

    /* compare md5 chksum */
    if (outputfile.checksumSize != -1 &&
        digest != outputfile.checksum)
    {
        if (debug)
        {
            fprintf(debug, "chksum for %s: ", outputfile.filename);
            for (int j = 0; j < 16; j++)
            {
                fprintf(debug, "%02x", digest[j]);
            }
            fprintf(debug, "\n");
        }
        auto message =
            formatString("Checksum wrong for '%s'. The file has been replaced "
                         "or its contents have been modified. Cannot do appending "
                         "because of this condition.", outputfile.filename);
        GMX_THROW(InconsistentInputError(message));
    }
}

/*! \brief If supported, obtain a write lock on the log file.
 *
 * This wil prevent e.g. other mdrun instances from changing it while
 * we attempt to restart with appending. */
void lockLogFile(t_fileio   *logfio,
                 const char *logFilename)
{
    /* Note that there are systems where the lock operation
     * will succeed, but a second process can also lock the file.
     * We should probably try to detect this.
     */
#if defined __native_client__
    errno = ENOSYS;
    if (true)
#elif GMX_NATIVE_WINDOWS
    if (_locking(fileno(gmx_fio_getfp(logfio)), _LK_NBLCK, LONG_MAX) == -1)
#else
    // don't initialize here: the struct order is OS dependent!
    struct flock fl;
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    fl.l_pid    = 0;

    if (fcntl(fileno(gmx_fio_getfp(logfio)), F_SETLK, &fl) == -1)
#endif
    {
        if (errno == ENOSYS)
        {
            std::string message = "File locking is not supported on this system. "
                "Use mdrun -noappend to restart.";
            GMX_THROW(FileIOError(message));
        }
        else if (errno == EACCES || errno == EAGAIN)
        {
            auto message =
                formatString("Failed to lock: %s. Already running "
                             "simulation?", logFilename);
            GMX_THROW(FileIOError(message));
        }
        else
        {
            auto message =
                formatString("Failed to lock: %s. %s.",
                             logFilename, std::strerror(errno));
            GMX_THROW(FileIOError(message));
        }
    }
}

/*! \brief Prepare to append to output files.
 *
 * We use the file pointer positions of the output files stored in the
 * checkpoint file and truncate the files such that any frames written
 * after the checkpoint time are removed.  All files are md5sum
 * checked such that we can be sure that we do not truncate other
 * (maybe important) files. The log file is locked so that we can
 * avoid cases where another mdrun instance might still be writing to
 * the file. */
void
prepareForAppending(const ArrayRef<const gmx_file_position_t> outputFiles,
                    t_fileio                                 *logfio)
{
    if (GMX_FAHCORE)
    {
        // Can't check or truncate output files in general
        // TODO do we do this elsewhere for GMX_FAHCORE?
        return;
    }

    // Handle the log file separately - it comes first in the list
    // because we have already opened the log file. This ensures that
    // we retain a lock on the open file that is never lifted after
    // the checksum is calculated.
    const gmx_file_position_t &logOutputFile = outputFiles[0];
    lockLogFile(logfio, logOutputFile.filename);
    checkOutputFile(logfio, logOutputFile);

    if (gmx_fio_seek(logfio, logOutputFile.offset) != 0)
    {
        auto message =
            formatString("Seek error! Failed to truncate log file: %s.",
                         std::strerror(errno));
        GMX_THROW(FileIOError(message));
    }

    // Now handle the remaining outputFiles
    for (const auto &outputFile : outputFiles.subArray(1, outputFiles.size()-1))
    {
        t_fileio *fileToCheck = gmx_fio_open(outputFile.filename, "r+");
        checkOutputFile(fileToCheck, outputFile);
        gmx_fio_close(fileToCheck);

        if (GMX_NATIVE_WINDOWS)
        {
            // Can't truncate output files on this platform
            continue;
        }

        if (gmx_truncate(outputFile.filename, outputFile.offset) != 0)
        {
            auto message =
                formatString("Truncation of file %s failed. Cannot do appending "
                             "because of this failure.", outputFile.filename);
            GMX_THROW(FileIOError(message));
        }
    }
}

}   // namespace

std::tuple<StartingBehavior, LogFilePtr>
handleRestart(t_commrec              *cr,
              const gmx_multisim_t   *ms,
              const AppendingBehavior appendingBehavior,
              const int               nfile,
              t_filenm                fnm[])
{
    StartingBehavior startingBehavior;
    LogFilePtr       logFileGuard = nullptr;

    // Make sure all ranks agree on whether the (multi-)simulation can
    // proceed.
    int                numErrorsFound = 0;
    std::exception_ptr exceptionPtr;

    // Only the master rank of each simulation can do anything with
    // output files, so it is the only one that needs to consider
    // whether a restart might take place, and how to implement it.
    if (MASTER(cr))
    {
        try
        {
            CheckpointHeaderContents         headerContents;
            std::vector<gmx_file_position_t> outputFiles;

            std::tie(startingBehavior, headerContents, outputFiles) = chooseStartingBehavior(appendingBehavior, nfile, fnm);

            if (isMultiSim(ms))
            {
                // Multi-simulation restarts require that each
                // checkpoint describes the same simulation part. If
                // those don't match, then the simulation cannot
                // proceed, and can only report a diagnostic to
                // stderr. Only one simulation should do that.
                FILE *fpmulti = isMasterSim(ms) ? stderr : nullptr;
                check_multi_int(fpmulti, ms, headerContents.simulation_part, "simulation part", TRUE);
            }

            if (startingBehavior == StartingBehavior::RestartWithAppending)
            {
                logFileGuard = openLogFile(ftp2fn(efLOG, nfile, fnm),
                                           startingBehavior == StartingBehavior::RestartWithAppending);
                prepareForAppending(outputFiles, logFileGuard.get());
            }
            else
            {
                if (startingBehavior == StartingBehavior::RestartWithoutAppending)
                {
                    std::string suffix = formatString(".part%04d", headerContents.simulation_part + 1);
                    add_suffix_to_output_names(fnm, nfile, suffix.c_str());
                }
                logFileGuard = openLogFile(ftp2fn(efLOG, nfile, fnm),
                                           startingBehavior == StartingBehavior::RestartWithAppending);
            }
        }
        catch (const std::exception & /*ex*/)
        {
            exceptionPtr   = std::current_exception();
            numErrorsFound = 1;
        }
    }
    // Since the master rank (perhaps of only one simulation) may have
    // found an error condition, we now coordinate the behavior across
    // all ranks. However, only the applicable ranks will throw a
    // non-default exception.
    //
    // TODO Evolve some re-usable infrastructure for this, because it
    // will be needed in many places while setting up simulations.
    if (PAR(cr))
    {
        gmx_sumi(1, &numErrorsFound, cr);
    }
    if (isMultiSim(ms))
    {
        gmx_sumi_sim(1, &numErrorsFound, ms);
        if (PAR(cr))
        {
            gmx_bcast(sizeof(numErrorsFound), &numErrorsFound, cr);
        }
    }

    // Throw in a globally coordinated way, if needed
    if (numErrorsFound > 0)
    {
        if (exceptionPtr)
        {
            std::rethrow_exception(exceptionPtr);
        }
        else
        {
            GMX_THROW(ParallelConsistencyError("Another MPI rank encountered an exception"));
        }
    }
    if (PAR(cr))
    {
        gmx_bcast(sizeof(startingBehavior), &startingBehavior, cr);
    }

    return std::make_tuple(startingBehavior, std::move(logFileGuard));
}

} // namespace gmx
