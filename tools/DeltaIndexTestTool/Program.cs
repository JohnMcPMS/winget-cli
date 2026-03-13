// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

namespace DeltaIndexTestTool
{
    using LibGit2Sharp;
    using Microsoft.WinGetUtil.Api;
    using Microsoft.WinGetUtil.Interfaces;
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Linq;
    using System.Text;

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
                    case "--help":
                    case "-h":
                        PrintUsage();
                        return 0;
                }
            }

            if (string.IsNullOrEmpty(repoPath) || string.IsNullOrEmpty(outputDir))
            {
                PrintUsage();
                return 1;
            }

            if (!Directory.Exists(repoPath))
            {
                Console.Error.WriteLine($"Repository path does not exist: {repoPath}");
                return 1;
            }

            // Validate resume arguments: both must be provided together
            bool hasResume = !string.IsNullOrEmpty(resumeCommit) || !string.IsNullOrEmpty(resumeWorkingIndexPath);
            if (hasResume)
            {
                if (string.IsNullOrEmpty(resumeCommit) || string.IsNullOrEmpty(resumeWorkingIndexPath))
                {
                    Console.Error.WriteLine("--resume-commit and --resume-working-index must be provided together.");
                    return 1;
                }
                if (!File.Exists(resumeWorkingIndexPath))
                {
                    Console.Error.WriteLine($"Resume working index not found: {resumeWorkingIndexPath}");
                    return 1;
                }
            }

            Directory.CreateDirectory(outputDir);

            try
            {
                RunAnalysis(repoPath, outputDir, branch, intervalDays, maxCheckpoints,
                    resumeCommit, resumeWorkingIndexPath);
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
            Console.WriteLine("  --resume-commit <sha>           Commit SHA to resume from (skip initial build)");
            Console.WriteLine("  --resume-working-index <path>   Path to pre-packaging working index for resume commit");
            Console.WriteLine();
            Console.WriteLine("Resume: both --resume-* options must be provided together. The tool will");
            Console.WriteLine("  package the working index to produce checkpoint 0's full index, then");
            Console.WriteLine("  continue processing subsequent checkpoints from there.");
            Console.WriteLine();
            Console.WriteLine("Output:");
            Console.WriteLine("  results.csv        CSV of checkpoint sizes");
            Console.WriteLine("  report.html        HTML report with comparison chart");
        }

        static void RunAnalysis(string repoPath, string outputDir, string branch, int intervalDays, int maxCheckpoints,
            string resumeCommit, string resumeWorkingIndexPath)
        {
            Console.WriteLine($"Opening repository at: {repoPath}");
            Console.WriteLine($"Output directory: {outputDir}");
            Console.WriteLine($"Interval: every {intervalDays} day(s)");

            bool isResume = !string.IsNullOrEmpty(resumeCommit);

            // When resuming, find the resume commit to use as anchor; checkpoints start after it.
            // When --max is given without resume, work backward from HEAD to select the N most recent intervals.
            var checkpoints = SelectCheckpoints(repoPath, branch, intervalDays, maxCheckpoints, isResume ? resumeCommit : null);
            Console.WriteLine($"Selected {checkpoints.Count} checkpoints");

            if (checkpoints.Count == 0)
            {
                Console.Error.WriteLine("No checkpoints found.");
                return;
            }

            string workingIndexPath = Path.Combine(outputDir, "working_index.db");
            var results = new List<CheckpointResult>();
            var factory = new WinGetFactory();
            IWinGetSQLiteIndex? workingIndex = null;

            try
            {
                for (int i = 0; i < checkpoints.Count; i++)
                {
                    var checkpoint = checkpoints[i];
                    Console.WriteLine($"\n[{i + 1}/{checkpoints.Count}] Processing checkpoint: {checkpoint.Commit.Sha[..8]} ({checkpoint.Date:yyyy-MM-dd})");

                    var result = new CheckpointResult
                    {
                        Index = i,
                        Date = checkpoint.Date,
                        CommitSha = checkpoint.Commit.Sha[..8],
                    };

                    string checkpointDir = Path.Combine(outputDir, $"checkpoint_{i:D4}");
                    Directory.CreateDirectory(checkpointDir);

                    string fullIndexPath = Path.Combine(checkpointDir, "full_index.db");
                    string deltaPath = Path.Combine(checkpointDir, "delta.db");

                    if (i == 0 && isResume)
                    {
                        // Resume: copy the provided working index into position, then package it
                        // to produce this checkpoint's full index — same as a normal first checkpoint
                        // except we already have the working state.
                        Console.WriteLine($"  Resuming from provided working index (commit {resumeCommit[..Math.Min(8, resumeCommit.Length)]})");

                        File.Copy(resumeWorkingIndexPath, workingIndexPath, overwrite: true);

                        File.Copy(workingIndexPath, fullIndexPath, overwrite: true);
                        using (var packagingIndex = factory.SQLiteIndexOpen(fullIndexPath))
                        {
                            packagingIndex.PrepareForPackaging();
                        }

                        workingIndex = factory.SQLiteIndexOpen(workingIndexPath);
                        workingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);

                        result.FullIndexBytes = new FileInfo(fullIndexPath).Length;
                        result.DeltaBytes = 0;
                        result.PreviousFullIndexPath = null;
                        result.FullIndexPath = fullIndexPath;

                        Console.WriteLine($"  Full index: {result.FullIndexBytes / 1024.0 / 1024.0:F2} MB");
                    }
                    else if (i == 0)
                    {
                        // First checkpoint: build from scratch
                        Console.WriteLine("  Building initial full index from scratch...");

                        if (File.Exists(workingIndexPath)) File.Delete(workingIndexPath);

                        workingIndex = factory.SQLiteIndexCreate(workingIndexPath, 2u, 1u);
                        workingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, "0");

                        int added = AddAllManifests(workingIndex, repoPath, checkpoint.Commit, checkpointDir);
                        Console.WriteLine($"  Added {added} manifest files");

                        workingIndex.Dispose();
                        workingIndex = null;

                        File.Copy(workingIndexPath, fullIndexPath, overwrite: true);
                        using (var packagingIndex = factory.SQLiteIndexOpen(fullIndexPath))
                        {
                            packagingIndex.PrepareForPackaging();
                        }

                        workingIndex = factory.SQLiteIndexOpen(workingIndexPath);
                        workingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);

                        result.FullIndexBytes = new FileInfo(fullIndexPath).Length;
                        result.DeltaBytes = 0;
                        result.PreviousFullIndexPath = null;
                        result.FullIndexPath = fullIndexPath;

                        Console.WriteLine($"  Full index: {result.FullIndexBytes / 1024.0 / 1024.0:F2} MB");
                    }
                    else
                    {
                        // Subsequent checkpoint: apply git diff to working index
                        var prevCheckpoint = checkpoints[i - 1];
                        string prevFullIndexPath = results[i - 1].FullIndexPath!;

                        Console.WriteLine("  Applying git diff from previous checkpoint...");
                        int changed = ApplyGitDiff(workingIndex!, repoPath, prevCheckpoint.Commit, checkpoint.Commit, checkpointDir);
                        Console.WriteLine($"  Applied {changed} manifest changes");

                        workingIndex!.Dispose();
                        workingIndex = null;

                        // Build full index (no delta properties set)
                        string fullOnlyPath = fullIndexPath + ".full_only.db";
                        File.Copy(workingIndexPath, fullOnlyPath, overwrite: true);
                        using (var fullPackagingIndex = factory.SQLiteIndexOpen(fullOnlyPath))
                        {
                            fullPackagingIndex.PrepareForPackaging();
                        }
                        File.Move(fullOnlyPath, fullIndexPath, overwrite: true);

                        // Build delta index against previous full index
                        string deltaWorkPath = fullIndexPath + ".delta_work.db";
                        File.Copy(workingIndexPath, deltaWorkPath, overwrite: true);
                        using (var deltaPackagingIndex = factory.SQLiteIndexOpen(deltaWorkPath))
                        {
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.DeltaBaselineIndexPath, Path.GetFullPath(prevFullIndexPath));
                            deltaPackagingIndex.SetProperty(SQLiteIndexProperty.DeltaOutputPath, Path.GetFullPath(deltaPath));
                            deltaPackagingIndex.PrepareForPackaging();
                        }
                        File.Delete(deltaWorkPath);

                        workingIndex = factory.SQLiteIndexOpen(workingIndexPath);
                        workingIndex.SetProperty(SQLiteIndexProperty.PackageUpdateTrackingBaseTime, string.Empty);

                        result.FullIndexBytes = new FileInfo(fullIndexPath).Length;
                        result.DeltaBytes = File.Exists(deltaPath) ? new FileInfo(deltaPath).Length : 0;
                        result.PreviousFullIndexPath = prevFullIndexPath;
                        result.FullIndexPath = fullIndexPath;

                        Console.WriteLine($"  Full index: {result.FullIndexBytes / 1024.0 / 1024.0:F2} MB");
                        Console.WriteLine($"  Delta:      {result.DeltaBytes / 1024.0 / 1024.0:F2} MB");
                    }

                    results.Add(result);
                }
            }
            finally
            {
                workingIndex?.Dispose();
            }

            ComputeCumulativeSizes(results);

            string csvPath = Path.Combine(outputDir, "results.csv");
            WriteCsv(results, csvPath);
            Console.WriteLine($"\nResults written to: {csvPath}");

            string htmlPath = Path.Combine(outputDir, "report.html");
            WriteHtmlReport(results, htmlPath);
            Console.WriteLine($"Report written to: {htmlPath}");
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

            // Collect all commits sorted newest-first
            var allCommits = repo.Commits
                .QueryBy(new CommitFilter
                {
                    IncludeReachableFrom = branchRef.Tip,
                    SortBy = CommitSortStrategies.Time,
                })
                .ToList();

            if (allCommits.Count == 0) return [];

            // If resuming, find the anchor commit and exclude it and anything older
            DateTimeOffset? afterTime = null;
            if (!string.IsNullOrEmpty(afterCommitSha))
            {
                var anchor = allCommits.FirstOrDefault(c => c.Sha.StartsWith(afterCommitSha, StringComparison.OrdinalIgnoreCase));
                if (anchor == null)
                {
                    throw new InvalidOperationException($"Resume commit '{afterCommitSha}' not found on branch '{branch}'");
                }
                afterTime = anchor.Author.When;
            }

            if (maxCheckpoints > 0)
            {
                // Work backward from HEAD: pick intervals going back in time, then reverse
                var selected = new List<CommitCheckpoint>();
                DateTimeOffset? lastSelected = null;

                foreach (var commit in allCommits) // newest-first
                {
                    var commitTime = commit.Author.When;

                    // Skip commits at or before the resume anchor
                    if (afterTime.HasValue && commitTime <= afterTime.Value)
                        break;

                    if (lastSelected == null || (lastSelected.Value - commitTime).TotalDays >= intervalDays)
                    {
                        selected.Add(new CommitCheckpoint(commit, commitTime.DateTime));
                        lastSelected = commitTime;

                        if (selected.Count >= maxCheckpoints)
                            break;
                    }
                }

                // Return in chronological order (oldest first)
                selected.Reverse();
                return selected;
            }
            else
            {
                // Walk oldest-first, selecting at each interval
                var chronological = allCommits
                    .Where(c => !afterTime.HasValue || c.Author.When > afterTime.Value)
                    .Reverse()
                    .ToList();

                var selected = new List<CommitCheckpoint>();
                DateTimeOffset? lastSelected = null;

                foreach (var commit in chronological)
                {
                    var commitTime = commit.Author.When;

                    if (lastSelected == null || (commitTime - lastSelected.Value).TotalDays >= intervalDays)
                    {
                        selected.Add(new CommitCheckpoint(commit, commitTime.DateTime));
                        lastSelected = commitTime;
                    }
                }

                return selected;
            }
        }

        /// <summary>
        /// Extracts all YAML manifest files from a git commit to a temp directory and adds them to the index.
        /// Returns the count of manifests added.
        /// </summary>
        static int AddAllManifests(IWinGetSQLiteIndex index, string repoPath, Commit commit, string workDir)
        {
            string tempDir = Path.Combine(workDir, "manifests_full");
            if (Directory.Exists(tempDir)) Directory.Delete(tempDir, true);
            Directory.CreateDirectory(tempDir);

            using var repo = new Repository(repoPath);
            var manifestsDir = commit.Tree["manifests"];
            if (manifestsDir == null) return 0;

            int count = ExtractAndAddTree(index, repo, (Tree)manifestsDir.Target, tempDir, "manifests");
            return count;
        }

        static int ExtractAndAddTree(IWinGetSQLiteIndex index, Repository repo, Tree tree, string baseDir, string relativePath)
        {
            int count = 0;
            foreach (var entry in tree)
            {
                string entryRelPath = relativePath + "/" + entry.Name;
                if (entry.TargetType == TreeEntryTargetType.Tree)
                {
                    count += ExtractAndAddTree(index, repo, (Tree)entry.Target, baseDir, entryRelPath);
                }
                else if (entry.TargetType == TreeEntryTargetType.Blob && entry.Name.EndsWith(".yaml", StringComparison.OrdinalIgnoreCase))
                {
                    var blob = (Blob)entry.Target;
                    string localPath = Path.Combine(baseDir, entryRelPath.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(localPath)!);
                    File.WriteAllBytes(localPath, blob.GetContentStream().ReadAllBytes());

                    try
                    {
                        index.AddManifest(localPath, entryRelPath);
                        count++;
                    }
                    catch
                    {
                        // Dependency ordering issues — skip for now (same behavior as IndexCreationTool)
                    }
                }
            }
            return count;
        }

        /// <summary>
        /// Applies the git diff between two commits to the working index.
        /// Returns the total number of changed manifest entries.
        /// </summary>
        static int ApplyGitDiff(IWinGetSQLiteIndex index, string repoPath, Commit fromCommit, Commit toCommit, string workDir)
        {
            string tempDir = Path.Combine(workDir, "manifests_diff");
            if (Directory.Exists(tempDir)) Directory.Delete(tempDir, true);
            Directory.CreateDirectory(tempDir);

            using var repo = new Repository(repoPath);

            var diff = repo.Diff.Compare<TreeChanges>(fromCommit.Tree, toCommit.Tree);
            int count = 0;

            foreach (var change in diff)
            {
                // Only process YAML files under manifests/
                if (!change.Path.StartsWith("manifests/", StringComparison.OrdinalIgnoreCase) ||
                    !change.Path.EndsWith(".yaml", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                try
                {
                    switch (change.Status)
                    {
                        case ChangeKind.Added:
                        {
                            string localPath = ExtractBlobToTemp(repo, toCommit, change.Path, tempDir);
                            index.AddManifest(localPath, change.Path);
                            count++;
                            break;
                        }
                        case ChangeKind.Modified:
                        case ChangeKind.Renamed:
                        {
                            string localPath = ExtractBlobToTemp(repo, toCommit, change.Path, tempDir);
                            index.UpdateManifest(localPath, change.Path);
                            count++;
                            break;
                        }
                        case ChangeKind.Deleted:
                        {
                            // For removal we need the old content to get the package ID
                            string localPath = ExtractBlobToTemp(repo, fromCommit, change.OldPath, tempDir);
                            index.RemoveManifest(localPath, change.OldPath);
                            count++;
                            break;
                        }
                    }
                }
                catch
                {
                    // Skip manifest processing errors (e.g., dependency issues)
                }
            }

            return count;
        }

        static string ExtractBlobToTemp(Repository repo, Commit commit, string path, string tempDir)
        {
            var entry = commit[path];
            var blob = (Blob)entry.Target;
            string localPath = Path.Combine(tempDir, path.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(localPath)!);
            File.WriteAllBytes(localPath, blob.GetContentStream().ReadAllBytes());
            return localPath;
        }

        static void ComputeCumulativeSizes(List<CheckpointResult> results)
        {
            long cumulativeFull = 0;
            long cumulativeDelta = 0;

            for (int i = 0; i < results.Count; i++)
            {
                cumulativeFull += results[i].FullIndexBytes;
                results[i].CumulativeFullDownloadBytes = cumulativeFull;

                if (i == 0)
                {
                    cumulativeDelta += results[i].FullIndexBytes; // First checkpoint: must download full
                }
                else
                {
                    cumulativeDelta += results[i].DeltaBytes; // Subsequent: download delta only
                }
                results[i].CumulativeDeltaDownloadBytes = cumulativeDelta;
            }
        }

        static void WriteCsv(List<CheckpointResult> results, string path)
        {
            using var writer = new StreamWriter(path, false, Encoding.UTF8);
            writer.WriteLine("Index,Date,CommitSha,FullIndexMB,DeltaMB,CumulativeFullMB,CumulativeDeltaMB,SavingsPercent");

            foreach (var r in results)
            {
                double fullMb = r.FullIndexBytes / 1024.0 / 1024.0;
                double deltaMb = r.DeltaBytes / 1024.0 / 1024.0;
                double cumFullMb = r.CumulativeFullDownloadBytes / 1024.0 / 1024.0;
                double cumDeltaMb = r.CumulativeDeltaDownloadBytes / 1024.0 / 1024.0;
                double savings = r.CumulativeFullDownloadBytes > 0
                    ? 100.0 * (1.0 - (double)r.CumulativeDeltaDownloadBytes / r.CumulativeFullDownloadBytes)
                    : 0;

                writer.WriteLine($"{r.Index},{r.Date:yyyy-MM-dd},{r.CommitSha},{fullMb:F2},{deltaMb:F2},{cumFullMb:F2},{cumDeltaMb:F2},{savings:F1}");
            }
        }

        static void WriteHtmlReport(List<CheckpointResult> results, string path)
        {
            var sb = new StringBuilder();
            sb.AppendLine("<!DOCTYPE html><html><head><meta charset='utf-8'>");
            sb.AppendLine("<title>Delta Index Size Analysis</title>");
            sb.AppendLine("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>");
            sb.AppendLine("<style>body{font-family:sans-serif;margin:20px;} table{border-collapse:collapse;width:100%;} th,td{border:1px solid #ddd;padding:8px;text-align:right;} th{background:#4472C4;color:white;} tr:nth-child(even){background:#f2f2f2;} canvas{max-width:900px;margin:20px 0;}</style>");
            sb.AppendLine("</head><body>");
            sb.AppendLine("<h1>Delta Index Size Analysis</h1>");
            sb.AppendLine("<h2>Cumulative Download: Full Strategy vs Delta Strategy</h2>");
            sb.AppendLine("<canvas id='chart'></canvas>");
            sb.AppendLine("<script>");
            sb.AppendLine("const labels = [" + string.Join(",", results.Select(r => $"'{r.Date:yyyy-MM-dd}'")) + "];");
            sb.AppendLine("const fullData = [" + string.Join(",", results.Select(r => (r.CumulativeFullDownloadBytes / 1024.0 / 1024.0).ToString("F2"))) + "];");
            sb.AppendLine("const deltaData = [" + string.Join(",", results.Select(r => (r.CumulativeDeltaDownloadBytes / 1024.0 / 1024.0).ToString("F2"))) + "];");
            sb.AppendLine(@"
new Chart(document.getElementById('chart'), {
  type: 'line',
  data: {
    labels: labels,
    datasets: [
      { label: 'Always Full (MB)', data: fullData, borderColor: '#C0392B', tension: 0.1 },
      { label: 'Baseline+Delta (MB)', data: deltaData, borderColor: '#27AE60', tension: 0.1 }
    ]
  },
  options: { responsive: true, scales: { y: { title: { display: true, text: 'Cumulative Download (MB)' } } } }
});");
            sb.AppendLine("</script>");

            // Summary table
            sb.AppendLine("<h2>Per-Checkpoint Details</h2>");
            sb.AppendLine("<table><tr><th>Index</th><th>Date</th><th>Commit</th><th>Full Index (MB)</th><th>Delta (MB)</th><th>Cum. Full (MB)</th><th>Cum. Delta (MB)</th><th>Savings (%)</th></tr>");

            foreach (var r in results)
            {
                double savings = r.CumulativeFullDownloadBytes > 0
                    ? 100.0 * (1.0 - (double)r.CumulativeDeltaDownloadBytes / r.CumulativeFullDownloadBytes)
                    : 0;

                sb.AppendLine($"<tr><td>{r.Index}</td><td>{r.Date:yyyy-MM-dd}</td><td>{r.CommitSha}</td>" +
                    $"<td>{r.FullIndexBytes / 1024.0 / 1024.0:F2}</td>" +
                    $"<td>{r.DeltaBytes / 1024.0 / 1024.0:F2}</td>" +
                    $"<td>{r.CumulativeFullDownloadBytes / 1024.0 / 1024.0:F2}</td>" +
                    $"<td>{r.CumulativeDeltaDownloadBytes / 1024.0 / 1024.0:F2}</td>" +
                    $"<td>{savings:F1}%</td></tr>");
            }

            sb.AppendLine("</table>");
            sb.AppendLine("</body></html>");
            File.WriteAllText(path, sb.ToString(), Encoding.UTF8);
        }
    }

    record CommitCheckpoint(Commit Commit, DateTime Date);

    class CheckpointResult
    {
        public int Index { get; set; }
        public DateTime Date { get; set; }
        public string CommitSha { get; set; } = string.Empty;
        public long FullIndexBytes { get; set; }
        public long DeltaBytes { get; set; }
        public long CumulativeFullDownloadBytes { get; set; }
        public long CumulativeDeltaDownloadBytes { get; set; }
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
