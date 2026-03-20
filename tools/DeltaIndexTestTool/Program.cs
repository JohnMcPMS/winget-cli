// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

namespace DeltaIndexTestTool
{
    using LibGit2Sharp;
    using Microsoft.WinGetUtil.Api;
    using Microsoft.WinGetUtil.Interfaces;
    using System;
    using System.Collections.Generic;
    using System.Diagnostics;
    using System.IO;
    using System.Linq;
    using System.Text;
    using System.Text.Json;
    using System.Text.Json.Serialization;

    /// <summary>
    /// Walks the git history of a winget-pkgs clone at weekly intervals, building a full V2 index
    /// and a delta index at each checkpoint, then reports the cumulative download size comparison
    /// between always-downloading-full vs downloading-baseline-plus-deltas strategies.
    /// </summary>
    class Program
    {
        static int Main(string[] args)
        {
            string repoPath = string.Empty;
            string outputDir = string.Empty;
            int intervalDays = 7;
            int maxCheckpoints = 0;
            string branch = "master";
            string resumeCommit = string.Empty;
            string resumeWorkingIndexPath = string.Empty;
            bool autoResume = false;

            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--repo" when i + 1 < args.Length:
                        repoPath = args[++i];
                        break;
                    case "--output" when i + 1 < args.Length:
                        outputDir = args[++i];
                        break;
                    case "--interval" when i + 1 < args.Length:
                        intervalDays = int.Parse(args[++i]);
                        break;
                    case "--max" when i + 1 < args.Length:
                        maxCheckpoints = int.Parse(args[++i]);
                        break;
                    case "--branch" when i + 1 < args.Length:
                        branch = args[++i];
                        break;
                    case "--resume-commit" when i + 1 < args.Length:
                        resumeCommit = args[++i];
                        break;
                    case "--resume-working-index" when i + 1 < args.Length:
                        resumeWorkingIndexPath = args[++i];
                        break;
                    case "--resume":
                        autoResume = true;
                        break;
                    case "--help":
                    case "-h":
                        PrintUsage();
                        return 0;
                    default:
                        PrintUsage();
                        return 1;
                }
            }

            if (string.IsNullOrEmpty(outputDir))
            {
                PrintUsage();
                return 1;
            }

            if (!autoResume)
            {
                if (string.IsNullOrEmpty(repoPath))
                {
                    PrintUsage();
                    return 1;
                }

                if (!Directory.Exists(repoPath))
                {
                    Console.Error.WriteLine($"Repository path does not exist: {repoPath}");
                    return 1;
                }
            }

            // --resume-working-index requires --resume-commit; the reverse is fine (build from scratch at that commit)
            if (!string.IsNullOrEmpty(resumeWorkingIndexPath) && string.IsNullOrEmpty(resumeCommit))
            {
                Console.Error.WriteLine("--resume-working-index requires --resume-commit.");
                return 1;
            }
            if (!string.IsNullOrEmpty(resumeWorkingIndexPath) && !File.Exists(resumeWorkingIndexPath))
            {
                Console.Error.WriteLine($"Resume working index not found: {resumeWorkingIndexPath}");
                return 1;
            }
            if (autoResume && (!string.IsNullOrEmpty(resumeCommit) || !string.IsNullOrEmpty(resumeWorkingIndexPath)))
            {
                Console.Error.WriteLine("--resume cannot be combined with --resume-commit or --resume-working-index.");
                return 1;
            }
            if (autoResume && !File.Exists(Path.Combine(outputDir, "state.json")))
            {
                Console.Error.WriteLine($"No state.json found in output directory. Run without --resume first.");
                return 1;
            }

            Directory.CreateDirectory(outputDir);

            try
            {
                RunAnalysis(repoPath, outputDir, branch, intervalDays, maxCheckpoints,
                    resumeCommit, resumeWorkingIndexPath, autoResume);
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Error: {ex.Message}");
                Console.Error.WriteLine(ex.StackTrace);
                return 1;
            }
        }

        static void PrintUsage()
        {
            Console.WriteLine("DeltaIndexTestTool - Measures delta index size vs full index size over git history");
            Console.WriteLine();
            Console.WriteLine("Usage: DeltaIndexTestTool --repo <path> --output <dir> [options]");
            Console.WriteLine();
            Console.WriteLine("Options:");
            Console.WriteLine("  --repo <path>                   Path to local winget-pkgs git clone");
            Console.WriteLine("  --output <dir>                  Directory to write results and index files");
            Console.WriteLine("  --interval <days>               Interval between checkpoints in days (default: 7)");
            Console.WriteLine("  --max <n>                       Maximum number of checkpoints; selects the N most");
            Console.WriteLine("                                  recent intervals working backward from HEAD");
            Console.WriteLine("  --branch <name>                 Branch to walk (default: master)");
            Console.WriteLine("  --resume                        Resume from the last complete checkpoint recorded in");
            Console.WriteLine("                                  state.json (requires prior run with same --output dir).");
            Console.WriteLine("                                  Cannot be combined with --resume-commit.");
            Console.WriteLine("  --resume-commit <sha>           Commit SHA to resume from");
            Console.WriteLine("  --resume-working-index <path>   Path to pre-packaging working index for resume commit");
            Console.WriteLine();
            Console.WriteLine("Resume modes:");
            Console.WriteLine("  --resume                        Reads state.json from the output directory, finds the");
            Console.WriteLine("                                  first incomplete checkpoint, and continues from there.");
            Console.WriteLine("  --resume-commit only            Starts a fresh index at that commit, then continues");
            Console.WriteLine("                                  forward from there (skips re-walking older history).");
            Console.WriteLine("  --resume-commit + --resume-working-index");
            Console.WriteLine("                                  Uses the provided pre-built working index as checkpoint 0,");
            Console.WriteLine("                                  packages it, then continues with subsequent checkpoints.");
            Console.WriteLine("  --resume-working-index requires --resume-commit.");
            Console.WriteLine();
            Console.WriteLine("Output:");
            Console.WriteLine("  state.json         Run parameters and last complete checkpoint index (auto-managed)");
            Console.WriteLine("  results.csv        CSV of checkpoint sizes");
        }

        static void RunAnalysis(string repoPath, string outputDir, string branch, int intervalDays, int maxCheckpoints,
            string resumeCommit, string resumeWorkingIndexPath, bool autoResume)
        {
            Console.WriteLine($"Output directory: {outputDir}");

            string stateFilePath = Path.Combine(outputDir, "state.json");
            bool hasResumeCommit = !string.IsNullOrEmpty(resumeCommit);

            List<CommitCheckpoint> checkpoints;
            ToolState state;

            if (autoResume)
            {
                state = LoadState(stateFilePath);
                checkpoints = state.Checkpoints.Select(c => new CommitCheckpoint(c.Sha, c.Date)).ToList();
                int startIndex = state.LastCompleteIndex + 1;

                Console.WriteLine($"Opening repository at: {state.RepoPath}");
                Console.WriteLine($"Interval: every {state.IntervalDays} day(s)");
                Console.WriteLine($"Checkpoints: {checkpoints.Count} total, resuming from {startIndex}");

                if (startIndex >= checkpoints.Count)
                {
                    Console.WriteLine("All checkpoints are already complete. Nothing to do.");
                    // Re-write the CSV with all existing results so the output is consistent.
                    var allResults = ReconstructResults(outputDir, checkpoints);
                    WriteCsv(allResults, Path.Combine(outputDir, "results.csv"));
                    return;
                }
            }
            else
            {
                Console.WriteLine($"Opening repository at: {repoPath}");
                Console.WriteLine($"Interval: every {intervalDays} day(s)");

                checkpoints = SelectCheckpoints(repoPath, branch, intervalDays, maxCheckpoints,
                    hasResumeCommit ? resumeCommit : null);

                if (hasResumeCommit)
                {
                    var resumeDate = LookupCommitDate(repoPath, resumeCommit);
                    checkpoints.Insert(0, new CommitCheckpoint(resumeCommit, resumeDate));
                }

                if (checkpoints.Count == 0)
                {
                    Console.Error.WriteLine("No checkpoints found.");
                    return;
                }

                state = new ToolState
                {
                    StateFilePath = stateFilePath,
                    RepoPath = repoPath,
                    Branch = branch,
                    IntervalDays = intervalDays,
                    Checkpoints = checkpoints.Select(c => new CheckpointRecord { Sha = c.Sha, Date = c.Date }).ToList(),
                    LastCompleteIndex = -1,
                };
                SaveState(state);
            }

            RunAnalysis(state, outputDir, resumeCommit, resumeWorkingIndexPath, checkpoints);
        }

        static void RunAnalysis(ToolState state, string outputDir, string resumeCommit, string resumeWorkingIndexPath, List<CommitCheckpoint> checkpoints)
        {
            Console.WriteLine($"Selected {checkpoints.Count} checkpoints");

            string workingIndexPath = Path.Combine(outputDir, "working_index.db");
            var results = new List<CheckpointResult>();
            var factory = new WinGetFactory();
            IWinGetSQLiteIndex? workingIndex = null;
            int startIndex = state.LastCompleteIndex + 1;
            bool hasResumeIndex = !string.IsNullOrEmpty(resumeWorkingIndexPath);

            // Pre-populate results for already-complete checkpoints.
            if (startIndex > 0)
            {
                results.AddRange(ReconstructResults(outputDir, checkpoints.Take(startIndex)));
            }

            // Open the working index from the last complete checkpoint when resuming mid-run.
            if (startIndex > 0)
            {
                string prevSavedPath = Path.Combine(outputDir, $"checkpoint_{startIndex - 1:D4}", "working_index.db");
                Console.WriteLine($"\nCopying working index from checkpoint {startIndex - 1}...");
                File.Copy(prevSavedPath, workingIndexPath, overwrite: true);
                workingIndex = factory.SQLiteIndexOpen(workingIndexPath);
            }

            try
            {
                for (int i = startIndex; i < checkpoints.Count; i++)
                {
                    var checkpoint = checkpoints[i];
                    Console.WriteLine($"\n[{i + 1}/{checkpoints.Count}] Processing checkpoint: {checkpoint.Sha[..8]} ({checkpoint.Date:yyyy-MM-dd})");

                    var result = new CheckpointResult
                    {
                        Index = i,
                        Date = checkpoint.Date,
                        CommitSha = checkpoint.Sha[..8],
                    };

                    string checkpointDir = Path.Combine(outputDir, $"checkpoint_{i:D4}");
                    Directory.CreateDirectory(checkpointDir);

                    string fullIndexPath = Path.Combine(checkpointDir, "full_index.db");
                    string deltaPrevPath = Path.Combine(checkpointDir, "delta_prev.db");
                    string deltaOrigPath = Path.Combine(checkpointDir, "delta_orig.db");

                    if (i == 0 && hasResumeIndex)
                    {
                        // Resume with a pre-built working index: copy it into position and package.
                        Console.WriteLine($"  Resuming from provided working index (commit {resumeCommit[..Math.Min(8, resumeCommit.Length)]})");

                        File.Copy(resumeWorkingIndexPath, workingIndexPath, overwrite: true);

                        string savedWorkingPath = Path.Combine(checkpointDir, "working_index.db");
                        File.Copy(workingIndexPath, savedWorkingPath, overwrite: true);

                        File.Copy(workingIndexPath, fullIndexPath, overwrite: true);
                        using (var packagingIndex = factory.SQLiteIndexOpen(fullIndexPath))
                        {
                            packagingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);
                            packagingIndex.PrepareForPackaging();
                        }

                        workingIndex = factory.SQLiteIndexOpen(workingIndexPath);

                        result.FullIndexBytes = new FileInfo(fullIndexPath).Length;
                        result.PreviousFullIndexPath = null;
                        result.FullIndexPath = fullIndexPath;

                        Console.WriteLine($"  Full index: {result.FullIndexBytes / 1024.0 / 1024.0:F2} MB");
                    }
                    else if (i == 0)
                    {
                        // First checkpoint with no pre-built index: build from scratch.
                        Console.WriteLine("  Building initial full index from scratch...");

                        if (File.Exists(workingIndexPath)) File.Delete(workingIndexPath);

                        workingIndex = factory.SQLiteIndexCreate(workingIndexPath, 2u, 1u);
                        workingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, "0");

                        int added = AddAllManifests(workingIndex, state.RepoPath, checkpoint.Sha);
                        Console.WriteLine($"  Added {added} manifest files");

                        workingIndex.Dispose();
                        workingIndex = null;

                        string savedWorkingPath = Path.Combine(checkpointDir, "working_index.db");
                        File.Copy(workingIndexPath, savedWorkingPath, overwrite: true);

                        File.Copy(workingIndexPath, fullIndexPath, overwrite: true);
                        using (var packagingIndex = factory.SQLiteIndexOpen(fullIndexPath))
                        {
                            packagingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);
                            packagingIndex.PrepareForPackaging();
                        }

                        workingIndex = factory.SQLiteIndexOpen(workingIndexPath);

                        result.FullIndexBytes = new FileInfo(fullIndexPath).Length;
                        result.PreviousFullIndexPath = null;
                        result.FullIndexPath = fullIndexPath;

                        Console.WriteLine($"  Full index: {result.FullIndexBytes / 1024.0 / 1024.0:F2} MB");
                    }
                    else
                    {
                        // Subsequent checkpoint: apply git diff to working index
                        var prevCheckpoint = checkpoints[i - 1];
                        string prevFullIndexPath = results[i - 1].FullIndexPath!;
                        string origFullIndexPath = results[0].FullIndexPath!;

                        Console.WriteLine("  Applying git diff from previous checkpoint...");
                        int changed = ApplyGitDiff(workingIndex!, state.RepoPath, prevCheckpoint.Sha, checkpoint.Sha, checkpointDir);
                        Console.WriteLine($"  Applied {changed} manifest changes");

                        workingIndex!.Dispose();
                        workingIndex = null;

                        string savedWorkingPath = Path.Combine(checkpointDir, "working_index.db");
                        File.Copy(workingIndexPath, savedWorkingPath, overwrite: true);

                        // Build full index (no delta properties set)
                        string fullOnlyPath = fullIndexPath + ".full_only.db";
                        File.Copy(workingIndexPath, fullOnlyPath, overwrite: true);
                        using (var fullPackagingIndex = factory.SQLiteIndexOpen(fullOnlyPath))
                        {
                            fullPackagingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);
                            fullPackagingIndex.PrepareForPackaging();
                        }
                        File.Move(fullOnlyPath, fullIndexPath, overwrite: true);

                        // Build delta index against previous full index
                        string deltaPrevWorkPath = fullIndexPath + ".delta_prev_cp.db";
                        File.Copy(workingIndexPath, deltaPrevWorkPath, overwrite: true);
                        using (var deltaPackagingIndex = factory.SQLiteIndexOpen(deltaPrevWorkPath))
                        {
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.DeltaBaselineIndexPath, Path.GetFullPath(prevFullIndexPath));
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.DeltaOutputPath, Path.GetFullPath(deltaPrevPath));
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);
                            deltaPackagingIndex.PrepareForPackaging();
                        }
                        File.Delete(deltaPrevWorkPath);

                        // Build delta index against original (checkpoint 0) full index
                        string deltaOrigWorkPath = fullIndexPath + ".delta_orig_cp.db";
                        File.Copy(workingIndexPath, deltaOrigWorkPath, overwrite: true);
                        using (var deltaPackagingIndex = factory.SQLiteIndexOpen(deltaOrigWorkPath))
                        {
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.DeltaBaselineIndexPath, Path.GetFullPath(origFullIndexPath));
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.DeltaOutputPath, Path.GetFullPath(deltaOrigPath));
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);
                            deltaPackagingIndex.PrepareForPackaging();
                        }
                        File.Delete(deltaOrigWorkPath);

                        workingIndex = factory.SQLiteIndexOpen(workingIndexPath);

                        result.FullIndexBytes = new FileInfo(fullIndexPath).Length;
                        result.DeltaPrevBytes = File.Exists(deltaPrevPath) ? new FileInfo(deltaPrevPath).Length : 0;
                        result.DeltaOrigBytes = File.Exists(deltaOrigPath) ? new FileInfo(deltaOrigPath).Length : 0;
                        result.PreviousFullIndexPath = prevFullIndexPath;
                        result.FullIndexPath = fullIndexPath;

                        Console.WriteLine($"  Full index: {result.FullIndexBytes / 1024.0 / 1024.0:F2} MB");
                        Console.WriteLine($"  Delta prev: {result.DeltaPrevBytes / 1024.0 / 1024.0:F2} MB");
                        Console.WriteLine($"  Delta orig: {result.DeltaOrigBytes / 1024.0 / 1024.0:F2} MB");
                    }

                    results.Add(result);

                    // Mark this checkpoint complete in the state file.
                    state.LastCompleteIndex = i;
                    SaveState(state);
                }
            }
            finally
            {
                workingIndex?.Dispose();
            }

            string csvPath = Path.Combine(outputDir, "results.csv");
            WriteCsv(results, csvPath);
            Console.WriteLine($"\nResults written to: {csvPath}");
        }

        // -----------------------------------------------------------------------
        // State file helpers
        // -----------------------------------------------------------------------

        static readonly JsonSerializerOptions s_jsonOptions = new()
        {
            WriteIndented = true,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        };

        static void SaveState(ToolState state)
        {
            File.WriteAllText(state.StateFilePath, JsonSerializer.Serialize(state, s_jsonOptions), Encoding.UTF8);
        }

        static ToolState LoadState(string path)
        {
            string json = File.ReadAllText(path, Encoding.UTF8);
            var result = JsonSerializer.Deserialize<ToolState>(json, s_jsonOptions)
                ?? throw new InvalidOperationException($"Failed to deserialize state file: {path}");
            result.StateFilePath = path;
            return result;
        }

        /// <summary>
        /// Rebuilds <see cref="CheckpointResult"/> objects for already-complete checkpoints
        /// by reading file sizes from disk.  Used when resuming a run.
        /// </summary>
        static List<CheckpointResult> ReconstructResults(string outputDir, IEnumerable<CommitCheckpoint> checkpoints)
        {
            var results = new List<CheckpointResult>();
            int i = 0;
            foreach (var cp in checkpoints)
            {
                string checkpointDir = Path.Combine(outputDir, $"checkpoint_{i:D4}");
                string fullIndexPath = Path.Combine(checkpointDir, "full_index.db");
                string deltaPrevPath = Path.Combine(checkpointDir, "delta_prev.db");
                string deltaOrigPath = Path.Combine(checkpointDir, "delta_orig.db");

                results.Add(new CheckpointResult
                {
                    Index = i,
                    Date = cp.Date,
                    CommitSha = cp.Sha[..8],
                    FullIndexBytes = File.Exists(fullIndexPath) ? new FileInfo(fullIndexPath).Length : 0,
                    DeltaPrevBytes = File.Exists(deltaPrevPath) ? new FileInfo(deltaPrevPath).Length : 0,
                    DeltaOrigBytes = File.Exists(deltaOrigPath) ? new FileInfo(deltaOrigPath).Length : 0,
                    FullIndexPath = fullIndexPath,
                    PreviousFullIndexPath = i > 0
                        ? Path.Combine(outputDir, $"checkpoint_{i - 1:D4}", "full_index.db")
                        : null,
                });
                i++;
            }
            return results;
        }

        static DateTime LookupCommitDate(string repoPath, string sha)
        {
            using var repo = new Repository(repoPath);
            var commit = repo.Lookup<Commit>(sha)
                ?? throw new InvalidOperationException($"Commit '{sha}' not found in repository");
            return commit.Author.When.DateTime;
        }

        /// <summary>
        /// Selects commits at evenly-spaced intervals across the branch history.
        ///
        /// When <paramref name="maxCheckpoints"/> is positive, selects the N most recent
        /// intervals working backward from HEAD, then returns them in chronological order.
        ///
        /// When <paramref name="afterCommitSha"/> is provided, only commits strictly after
        /// that commit are considered (used for resume mode).
        /// </summary>
        static List<CommitCheckpoint> SelectCheckpoints(string repoPath, string branch, int intervalDays, int maxCheckpoints, string? afterCommitSha)
        {
            using var repo = new Repository(repoPath);

            var branchRef = repo.Branches[branch] ?? repo.Branches[$"origin/{branch}"];
            if (branchRef == null)
            {
                throw new InvalidOperationException($"Branch '{branch}' not found in repository");
            }

            // Resolve the anchor commit's timestamp without loading all commits.
            // repo.Lookup<Commit> handles full and abbreviated SHAs efficiently.
            DateTimeOffset? afterTime = null;
            if (!string.IsNullOrEmpty(afterCommitSha))
            {
                var anchor = repo.Lookup<Commit>(afterCommitSha);
                if (anchor == null)
                {
                    throw new InvalidOperationException($"Resume commit '{afterCommitSha}' not found on branch '{branch}'");
                }
                afterTime = anchor.Author.When;
            }

            var filter = new CommitFilter
            {
                IncludeReachableFrom = branchRef.Tip,
                SortBy = CommitSortStrategies.Time,
            };

            // Stream commits newest-first; store only SHA strings to avoid holding native
            // libgit2 handles beyond the Repository lifetime.  Both the --max and no-max
            // paths work backward from HEAD and then reverse, which avoids a full
            // materialization of the 300K+ commit walk.
            var selected = new List<CommitCheckpoint>();
            DateTimeOffset? lastSelected = null;

            foreach (var commit in repo.Commits.QueryBy(filter)) // newest-first, lazy
            {
                var commitTime = commit.Author.When;

                // Stop as soon as we pass the resume anchor
                if (afterTime.HasValue && commitTime <= afterTime.Value)
                    break;

                if (lastSelected == null || (lastSelected.Value - commitTime).TotalDays >= intervalDays)
                {
                    selected.Add(new CommitCheckpoint(commit.Sha, commitTime.DateTime));
                    lastSelected = commitTime;

                    if (maxCheckpoints > 0 && selected.Count >= maxCheckpoints)
                        break;
                }
            }

            // Return in chronological order (oldest first)
            selected.Reverse();
            return selected;
        }

        /// <summary>
        /// Checks out the target commit in the repository, then walks the filesystem to collect
        /// manifest directories and add them to the index.  The repository is left at the target
        /// commit on return (no state is restored).
        /// Retries failures until no further progress can be made (resolves dependency ordering).
        /// Returns the count of manifests successfully added.
        /// </summary>
        static int AddAllManifests(IWinGetSQLiteIndex index, string repoPath, string commitSha)
        {
            Console.WriteLine($"    Checking out commit {commitSha[..8]}...");
            RunGit(repoPath, $"checkout --detach {commitSha}");

            string manifestsRoot = Path.Combine(repoPath, "manifests");
            if (!Directory.Exists(manifestsRoot)) return 0;

            // Collect manifest version directories: any directory that contains .yaml files directly.
            Console.WriteLine("    Collecting manifest directories from filesystem...");
            var manifests = Directory
                .EnumerateFiles(manifestsRoot, "*.yaml", SearchOption.AllDirectories)
                .GroupBy(f => Path.GetDirectoryName(f)!, StringComparer.OrdinalIgnoreCase)
                .Select(g => (
                    LocalDir: g.Key,
                    RelPath: Path.GetRelativePath(repoPath, g.Key).Replace(Path.DirectorySeparatorChar, '/')))
                .ToList();
            Console.WriteLine($"    Found {manifests.Count} manifest directories");

            // Initial add pass — collect failures, printing periodic progress.
            var failed = new List<(string LocalDir, string RelPath)>();
            int total = manifests.Count;
            int done = 0;
            foreach (var (localDir, relPath) in manifests)
            {
                try { index.AddManifest(localDir, relPath); }
                catch { failed.Add((localDir, relPath)); }

                done++;
                if (done % 500 == 0 || done == total)
                    Console.Write($"\r    Adding: {done}/{total} ({100.0 * done / total:F1}%)   ");
            }
            Console.WriteLine(); // end the \r line

            // Retry loop: keep going as long as at least one failure is resolved each round.
            int pass = 1;
            while (failed.Count > 0)
            {
                var retrying = failed;
                failed = [];
                Console.WriteLine($"    Retry pass {pass}: {retrying.Count} manifest(s) pending...");
                foreach (var (localDir, relPath) in retrying)
                {
                    try { index.AddManifest(localDir, relPath); }
                    catch { failed.Add((localDir, relPath)); }
                }

                // No progress this round — stop.
                if (failed.Count == retrying.Count) break;
                pass++;
            }

            foreach (var (_, relPath) in failed)
            {
                Console.Error.WriteLine($"  Could not add manifest (no progress): {relPath}");
            }

            return manifests.Count - failed.Count;
        }

        static void RunGit(string repoPath, string arguments)
        {
            var psi = new ProcessStartInfo("git")
            {
                Arguments = $"-C \"{repoPath}\" {arguments}",
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            using var p = Process.Start(psi)!;
            p.WaitForExit();
            if (p.ExitCode != 0)
            {
                string err = p.StandardError.ReadToEnd().Trim();
                throw new InvalidOperationException($"git {arguments} failed (exit {p.ExitCode}): {err}");
            }
        }

        static IEnumerable<string> RunGitLines(string repoPath, string arguments)
        {
            var psi = new ProcessStartInfo("git")
            {
                Arguments = $"-C \"{repoPath}\" {arguments}",
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            using var p = Process.Start(psi)!;
            string? line;
            while ((line = p.StandardOutput.ReadLine()) != null)
            {
                if (!string.IsNullOrEmpty(line))
                    yield return line;
            }
            p.WaitForExit();
            if (p.ExitCode != 0)
            {
                string err = p.StandardError.ReadToEnd().Trim();
                throw new InvalidOperationException($"git {arguments} failed (exit {p.ExitCode}): {err}");
            }
        }

        /// <summary>
        /// Walks every commit between <paramref name="fromSha"/> (exclusive) and
        /// <paramref name="toSha"/> (inclusive) in topological order (oldest first) using
        /// <c>git rev-list --topo-order --reverse</c>, then applies each commit's manifest
        /// change to the index.  Each commit is expected to touch at most one manifest directory.
        /// Returns the number of index operations that succeeded.
        /// </summary>
        static int ApplyGitDiff(IWinGetSQLiteIndex index, string repoPath, string fromSha, string toSha, string workDir)
        {
            string tempDir = Path.Combine(workDir, "manifests_diff");
            if (Directory.Exists(tempDir)) Directory.Delete(tempDir, true);
            Directory.CreateDirectory(tempDir);

            // Use git rev-list to obtain the definitive topological ordering (oldest-first).
            // This matches exactly what `git log --topo-order --reverse` produces and avoids
            // any ambiguity in LibGit2Sharp's CommitSortStrategies.Topological | Reverse.
            var commitShas = RunGitLines(repoPath, $"rev-list --topo-order --reverse {fromSha}..{toSha}")
                .ToList();

            if (commitShas.Count == 0) return 0;

            int count = 0;

            string logPath = Path.Combine(workDir, "manifest_operations.txt");
            using var log = new StreamWriter(logPath, append: false, Encoding.UTF8);
            log.WriteLine("Commit\tTimestamp\tOperation\tPath\tError");

            using var repo = new Repository(repoPath);

            foreach (var sha in commitShas)
            {
                var commit = repo.Lookup<Commit>(sha);
                var parent = commit.Parents.FirstOrDefault();
                var diff = repo.Diff.Compare<TreeChanges>(parent?.Tree, commit.Tree);

                // Collect per-directory changes within this commit.
                // A "move" commit deletes one version dir and adds another — both must be processed.
                // Key: directory path.  Value: (anyAdded, anyDeleted, anyModified).
                var dirChanges = new Dictionary<string, (bool AnyAdded, bool AnyDeleted, bool AnyModified)>(StringComparer.OrdinalIgnoreCase);

                foreach (var change in diff)
                {
                    // For renames, the old and new paths can be in different directories (e.g., a
                    // version bump moves manifests from /1.0.0/ to /2.0.0/).  We must record the
                    // deletion against the OLD directory and the addition against the NEW directory
                    // independently; collapsing both sides to a single path would cause the update
                    // path to look up the wrong tree when extracting the pre-commit state.

                    // Old side: record deletion in the source directory.
                    if (change.Status == ChangeKind.Deleted || change.Status == ChangeKind.Renamed)
                    {
                        string oldPath = change.OldPath;
                        if (oldPath.StartsWith("manifests/", StringComparison.OrdinalIgnoreCase) &&
                            oldPath.EndsWith(".yaml", StringComparison.OrdinalIgnoreCase))
                        {
                            int lastSlash = oldPath.LastIndexOf('/');
                            string dir = lastSlash > 0 ? oldPath[..lastSlash] : string.Empty;
                            dirChanges.TryGetValue(dir, out var flags);
                            dirChanges[dir] = (flags.AnyAdded, true, flags.AnyModified);
                        }
                    }

                    // New side: record addition/modification in the destination directory.
                    if (change.Status != ChangeKind.Deleted)
                    {
                        string newPath = change.Path;
                        if (newPath.StartsWith("manifests/", StringComparison.OrdinalIgnoreCase) &&
                            newPath.EndsWith(".yaml", StringComparison.OrdinalIgnoreCase))
                        {
                            int lastSlash = newPath.LastIndexOf('/');
                            string dir = lastSlash > 0 ? newPath[..lastSlash] : string.Empty;
                            dirChanges.TryGetValue(dir, out var flags);
                            dirChanges[dir] = (change.Status == ChangeKind.Added || change.Status == ChangeKind.Renamed)
                                ? (true,           flags.AnyDeleted, flags.AnyModified)
                                : (flags.AnyAdded, flags.AnyDeleted, true);
                        }
                    }
                }

                if (dirChanges.Count == 0) continue; // No manifest changes in this commit

                // Pure deletes first, then updates (remove+add), then pure adds.
                // This ensures move commits (delete old dir, add new dir) remove before adding.
                static int OpOrder((bool AnyAdded, bool AnyDeleted, bool AnyModified) f) =>
                    (!f.AnyAdded && !f.AnyModified) ? 0 :   // pure delete
                    (!f.AnyDeleted && !f.AnyModified) ? 2 :  // pure add
                    1;                                        // update (remove+add)

                foreach (var (dirPath, (anyAdded, anyDeleted, anyModified)) in dirChanges.OrderBy(kv => OpOrder(kv.Value)))
                {
                    bool isPureDelete = !anyAdded && !anyModified;
                    bool isPureAdd    = !anyDeleted && !anyModified;
                    // Everything else is an update: remove the old state then add the new state.

                    if (isPureDelete)
                    {
                        string localDir = ExtractManifestDirFromTree(repo, parent!, dirPath, tempDir);
                        TryIndexOp(index, log, commit, "remove", dirPath, localDir,
                            (idx, dir, path) => idx.RemoveManifest(dir, path), ref count);
                    }
                    else if (isPureAdd)
                    {
                        // If the directory already existed in the parent tree, this commit is
                        // adding new files to an existing manifest (e.g., a new locale yaml).
                        // The package is already in the index, so treat it as an update.
                        bool dirExistedInParent = parent != null &&
                            parent[dirPath]?.TargetType == TreeEntryTargetType.Tree;

                        if (dirExistedInParent)
                        {
                            string removeDir = ExtractManifestDirFromTree(repo, parent!, dirPath, tempDir);
                            TryIndexOp(index, log, commit, "remove", dirPath, removeDir,
                                (idx, dir, path) => idx.RemoveManifest(dir, path), ref count);

                            string addDir = ExtractManifestDirFromTree(repo, commit, dirPath, tempDir);
                            TryIndexOp(index, log, commit, "add", dirPath, addDir,
                                (idx, dir, path) => idx.AddManifest(dir, path), ref count);
                        }
                        else
                        {
                            string localDir = ExtractManifestDirFromTree(repo, commit, dirPath, tempDir);
                            TryIndexOp(index, log, commit, "add", dirPath, localDir,
                                (idx, dir, path) => idx.AddManifest(dir, path), ref count);
                        }
                    }
                    else
                    {
                        // Update: remove using pre-commit state, then add using post-commit state.
                        string removeDir = ExtractManifestDirFromTree(repo, parent!, dirPath, tempDir);
                        TryIndexOp(index, log, commit, "remove", dirPath, removeDir,
                            (idx, dir, path) => idx.RemoveManifest(dir, path), ref count);

                        string addDir = ExtractManifestDirFromTree(repo, commit, dirPath, tempDir);
                        TryIndexOp(index, log, commit, "add", dirPath, addDir,
                            (idx, dir, path) => idx.AddManifest(dir, path), ref count);
                    }
                }
            }

            Directory.Delete(tempDir, true);

            return count;
        }

        static void TryIndexOp(IWinGetSQLiteIndex index, StreamWriter log, Commit commit,
            string operationName, string dirPath, string localDir,
            Action<IWinGetSQLiteIndex, string, string> op, ref int count)
        {
            try
            {
                op(index, localDir, dirPath);
                log.WriteLine($"{commit.Sha}\t{commit.Author.When:yyyy-MM-dd HH:mm:ss zzz}\t{operationName}\t{dirPath}\t");
                count++;
            }
            catch (Exception ex)
            {
                int result = ex.InnerException?.HResult ?? ex.HResult;
                log.WriteLine($"{commit.Sha}\t{commit.Author.When:yyyy-MM-dd HH:mm:ss zzz}\t{operationName}\t{dirPath}\t{result}");
                Console.Error.WriteLine($"  Failed to {operationName} manifest '{dirPath}': {result}");
            }
        }

        /// <summary>
        /// Extracts all YAML files from the given <paramref name="dirPath"/> in the commit's tree
        /// into a subdirectory of <paramref name="tempDir"/> that includes the first 8 characters
        /// of the commit SHA, ensuring no cross-commit directory conflicts.
        /// Returns the local directory path used.
        /// </summary>
        static string ExtractManifestDirFromTree(Repository repo, Commit commit, string dirPath, string tempDir)
        {
            string localDir = Path.Combine(
                tempDir,
                dirPath.Replace('/', Path.DirectorySeparatorChar),
                commit.Sha[..8]);

            var entry = commit[dirPath];
            if (entry?.TargetType != TreeEntryTargetType.Tree) return localDir;

            Directory.CreateDirectory(localDir);
            foreach (var child in (Tree)entry.Target)
            {
                if (child.TargetType == TreeEntryTargetType.Blob &&
                    child.Name.EndsWith(".yaml", StringComparison.OrdinalIgnoreCase))
                {
                    var blob = (Blob)child.Target;
                    File.WriteAllBytes(Path.Combine(localDir, child.Name), blob.GetContentStream().ReadAllBytes());
                }
            }

            return localDir;
        }

        static void WriteCsv(List<CheckpointResult> results, string path)
        {
            using var writer = new StreamWriter(path, false, Encoding.UTF8);
            writer.WriteLine("Index,Date,CommitSha,FullIndexMB,DeltaPrevMB,DeltaOrigMB");

            foreach (var r in results)
            {
                double fullMb = r.FullIndexBytes / 1024.0 / 1024.0;
                double deltaPrevMb = r.DeltaPrevBytes / 1024.0 / 1024.0;
                double deltaOrigMb = r.DeltaOrigBytes / 1024.0 / 1024.0;

                writer.WriteLine($"{r.Index},{r.Date:yyyy-MM-dd},{r.CommitSha},{fullMb:F2},{deltaPrevMb:F2},{deltaOrigMb:F2}");
            }
        }
    }

    record CommitCheckpoint(string Sha, DateTime Date);

    /// <summary>
    /// Persisted run parameters and progress, written to state.json in the output directory.
    /// </summary>
    class ToolState
    {
        [JsonIgnore]
        public string StateFilePath { get; set; } = string.Empty;

        public string RepoPath { get; set; } = string.Empty;
        public string Branch { get; set; } = string.Empty;
        public int IntervalDays { get; set; } = 7;
        public List<CheckpointRecord> Checkpoints { get; set; } = new();

        /// <summary>
        /// Index of the last fully-completed checkpoint, or -1 if none have completed.
        /// Updated after each checkpoint succeeds; used by --resume to find the restart point.
        /// </summary>
        public int LastCompleteIndex { get; set; } = -1;
    }

    class CheckpointRecord
    {
        public string Sha { get; set; } = string.Empty;
        public DateTime Date { get; set; }
    }

    class CheckpointResult
    {
        public int Index { get; set; }
        public DateTime Date { get; set; }
        public string CommitSha { get; set; } = string.Empty;
        public long FullIndexBytes { get; set; }
        public long DeltaPrevBytes { get; set; }
        public long DeltaOrigBytes { get; set; }
        public string? FullIndexPath { get; set; }
        public string? PreviousFullIndexPath { get; set; }
    }

    static class StreamExtensions
    {
        public static byte[] ReadAllBytes(this Stream stream)
        {
            using var ms = new MemoryStream();
            stream.CopyTo(ms);
            return ms.ToArray();
        }
    }
}
