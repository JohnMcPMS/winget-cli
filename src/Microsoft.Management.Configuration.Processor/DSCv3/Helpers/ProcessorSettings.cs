// -----------------------------------------------------------------------------
// <copyright file="ProcessorSettings.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace Microsoft.Management.Configuration.Processor.DSCv3.Helpers
{
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Text;
    using Microsoft.Management.Configuration.Processor.DSCv3.Model;
    using Microsoft.Management.Configuration.Processor.Helpers;
    using Microsoft.Win32.SafeHandles;

    /// <summary>
    /// Contains settings for the DSC v3 processor components to share.
    /// </summary>
    internal class ProcessorSettings : IDisposable
    {
        private readonly object dscV3Lock = new ();
        private readonly object defaultPathLock = new ();
        private readonly object processorPathLock = new ();

        private FindDscPackageStateMachine dscPackageStateMachine = new ();
        private IDSCv3? dscV3 = null;
        private string? defaultPath = null;
        private string? defaultPathHash = null;
        private bool? defaultPathIsAlias = null;
        private SafeFileHandle? processorPathHandle = null;
        private bool processorPathVerified = false;
        private bool disposed = false;

        private Dictionary<string, ResourceDetails> resourceDetailsDictionary = new (StringComparer.OrdinalIgnoreCase);
        private ResourceCacheState cacheState = new ResourceCacheState();

        /// <summary>
        /// Gets or sets the path to the DSC v3 executable.
        /// </summary>
        public string? DscExecutablePath { get; set; }

        /// <summary>
        /// Gets or sets the expected SHA256 hash of the DSC v3 executable (hex string).
        /// Must be set before <see cref="EffectiveDscExecutablePath"/> is accessed when a custom path is used.
        /// </summary>
        public string? DscExecutablePathHash { get; set; }

        /// <summary>
        /// Gets or sets a value indicating whether <see cref="DscExecutablePath"/> is an app execution alias.
        /// </summary>
        public bool? DscExecutablePathIsAlias { get; set; }

        /// <summary>
        /// Gets the path to the DSC v3 executable.
        /// </summary>
        public string EffectiveDscExecutablePath
        {
            get
            {
                if (this.DscExecutablePath != null)
                {
                    this.EnsureProcessorPathVerified();
                    return this.DscExecutablePath;
                }

                lock (this.defaultPathLock)
                {
                    if (this.defaultPath != null)
                    {
                        return this.defaultPath;
                    }
                }

                string? localDefaultPath = this.GetFoundDscExecutablePath();

                if (localDefaultPath == null)
                {
                    throw new FileNotFoundException("Could not find DSC v3 executable path.");
                }

                lock (this.defaultPathLock)
                {
                    if (this.defaultPath == null)
                    {
                        this.defaultPath = localDefaultPath;
                    }

                    return this.defaultPath;
                }
            }
        }

        /// <summary>
        /// Gets an object for interacting with the DSC executable at EffectiveDscExecutablePath.
        /// </summary>
        [System.Diagnostics.CodeAnalysis.SuppressMessage("StyleCop.CSharp.DocumentationRules", "SA1623:Property summary documentation should match accessors", Justification = "Set is only provided for tests.")]
        public IDSCv3 DSCv3
        {
            get
            {
                lock (this.dscV3Lock)
                {
                    if (this.dscV3 == null)
                    {
                        this.dscV3 = IDSCv3.Create(this);
                    }

                    return this.dscV3;
                }
            }

#if !AICLI_DISABLE_TEST_HOOKS
            set
            {
                lock (this.dscV3Lock)
                {
                    this.dscV3 = value;
                }
            }
#endif
        }

        /// <summary>
        /// Gets or sets the diagnostics sink to use.
        /// </summary>
        public IDiagnosticsSink? DiagnosticsSink { get; set; } = null;

        /// <summary>
        /// Gets or sets a value indicating whether the processor should produce more verbose output.
        /// </summary>
        public bool DiagnosticTraceEnabled { get; set; } = false;

        /// <summary>
        /// Find the DSC v3 executable.
        /// </summary>
        /// <returns>The full path to the dsc.exe executable, or null if not found.</returns>
        public string? GetFoundDscExecutablePath()
        {
            string? result = this.dscPackageStateMachine.DscExecutablePath;

            if (result != null)
            {
                // Ensure hash and alias are computed and cached alongside the path.
                this.EnsureFoundPathHashCached(result);
            }

            return result;
        }

        /// <summary>
        /// Gets the SHA256 hash of the auto-discovered DSC executable path, or null if not yet discovered.
        /// </summary>
        /// <returns>Lowercase hex hash string, or null.</returns>
        public string? GetFoundDscExecutablePathHash()
        {
            lock (this.defaultPathLock)
            {
                return this.defaultPathHash;
            }
        }

        /// <summary>
        /// Gets whether the auto-discovered DSC executable path is an app execution alias, or null if not yet discovered.
        /// </summary>
        /// <returns>True if alias, false if regular file, or null if not yet discovered.</returns>
        public bool? GetFoundDscExecutablePathIsAlias()
        {
            lock (this.defaultPathLock)
            {
                return this.defaultPathIsAlias;
            }
        }

        /// <summary>
        /// Invokes a step in the DSC search state machine.
        /// </summary>
        /// <returns>The transition to take in the state machine.</returns>
        public FindDscPackageStateMachine.Transition PumpFindDscStateMachine()
        {
            return this.dscPackageStateMachine.DetermineNextTransition();
        }

        /// <inheritdoc/>
        public void Dispose()
        {
            this.Dispose(true);
            GC.SuppressFinalize(this);
        }

        /// <summary>
        /// Create a deep copy of this settings object.
        /// </summary>
        /// <returns>A deep copy of this object.</returns>
        public ProcessorSettings Clone()
        {
            ProcessorSettings result = new ProcessorSettings();

            result.resourceDetailsDictionary = this.resourceDetailsDictionary;
            result.cacheState = this.cacheState;
            result.DiagnosticsSink = this.DiagnosticsSink;
            result.DscExecutablePath = this.DscExecutablePath;
            result.DscExecutablePathHash = this.DscExecutablePathHash;
            result.DscExecutablePathIsAlias = this.DscExecutablePathIsAlias;
            result.DiagnosticTraceEnabled = this.DiagnosticTraceEnabled;
            lock (this.defaultPathLock)
            {
                result.defaultPath = this.defaultPath;
                result.defaultPathHash = this.defaultPathHash;
                result.defaultPathIsAlias = this.defaultPathIsAlias;
            }

#if !AICLI_DISABLE_TEST_HOOKS
            result.dscV3 = this.DSCv3;
#endif

            return result;
        }

        /// <summary>
        /// Gets a string representation of this object.
        /// </summary>
        /// <returns>A string representation of this object.</returns>
        public override string ToString()
        {
            StringBuilder sb = new StringBuilder();

            sb.Append("EffectiveDscExecutablePath: ");
            sb.AppendLine(this.EffectiveDscExecutablePath);

            sb.Append("DiagnosticTraceLevel: ");
            sb.Append(this.DiagnosticTraceEnabled);

            return sb.ToString();
        }

        /// <summary>
        /// Gets the ResourceDetails for a configuration unit.
        /// </summary>
        /// <param name="configurationUnitInternal">The configuration unit to find details for.</param>
        /// <param name="detailFlags">The level of detail to get.</param>
        /// <returns>The ResourceDetails for the unit, or null if not found.</returns>
        public ResourceDetails? GetResourceDetails(ConfigurationUnitInternal configurationUnitInternal, ConfigurationUnitDetailFlags detailFlags)
        {
            // Check cache first.
            ResourceDetails? result = this.TryGetFromCache(configurationUnitInternal.QualifiedName);

            if (result == null)
            {
                // Pre-populate cache with all top-level resources if not done yet.
                this.EnsureAllResourcesCached();
                result = this.TryGetFromCache(configurationUnitInternal.QualifiedName);
            }

            if (result == null)
            {
                // Not a top-level resource; check within each known adapter.
                List<string> adapterTypes = this.GetCachedAdapterTypes();

                foreach (string adapterType in adapterTypes)
                {
                    this.EnsureAdapterResourcesCached(adapterType);
                    result = this.TryGetFromCache(configurationUnitInternal.QualifiedName);
                    if (result != null)
                    {
                        break;
                    }
                }
            }

            if (result != null)
            {
                result.EnsureDetails(this, detailFlags);

                if (result.Exists)
                {
                    return result;
                }
            }

            return null;
        }

        /// <summary>
        /// Gets all ResourceDetails matching find options.
        /// </summary>
        /// <param name="findOptions">The find options.</param>
        /// <returns>A list of ResourceDetails.</returns>
        public List<ResourceDetails> FindAllResourceDetails(FindUnitProcessorsOptions findOptions)
        {
            List<ResourceDetails> result = new List<ResourceDetails>();

            var runSettings = ProcessorRunSettings.CreateFromFindUnitProcessorsOptions(findOptions);
            var resourceItemList = this.DSCv3.GetAllResources(runSettings);

            lock (this.resourceDetailsDictionary)
            {
                this.PopulateCacheFromResourceListUnderLock(resourceItemList);

                // If no custom search paths were used, the result represents the full default resource set.
                if (string.IsNullOrEmpty(findOptions.SearchPaths))
                {
                    this.cacheState.AllResourcesCached = true;
                }
            }

            foreach (var item in resourceItemList)
            {
                ResourceDetails? details;
                lock (this.resourceDetailsDictionary)
                {
                    this.resourceDetailsDictionary.TryGetValue(item.Type, out details);
                }

                if (details != null)
                {
                    details.EnsureDetails(this, findOptions.UnitDetailFlags);
                    result.Add(details);
                }
            }

            return result;
        }

        /// <summary>
        /// Releases resources held by this instance, including the open handle used for TOCTOU protection.
        /// </summary>
        /// <param name="disposing">True if called from Dispose(); false if called from a finalizer.</param>
        protected virtual void Dispose(bool disposing)
        {
            if (!this.disposed)
            {
                if (disposing)
                {
                    lock (this.processorPathLock)
                    {
                        this.processorPathHandle?.Dispose();
                        this.processorPathHandle = null;
                    }
                }

                this.disposed = true;
            }
        }

        private void EnsureFoundPathHashCached(string path)
        {
            lock (this.defaultPathLock)
            {
                if (this.defaultPathHash == null)
                {
                    this.defaultPathHash = ProcessorPathIntegrity.ComputeHash(path, out bool isAlias);
                    this.defaultPathIsAlias = isAlias;
                }
            }
        }

        private void EnsureProcessorPathVerified()
        {
            lock (this.processorPathLock)
            {
                if (!this.processorPathVerified)
                {
                    if (this.DscExecutablePathHash == null)
                    {
                        throw new InvalidOperationException("A custom processor path was provided without a hash for integrity verification.");
                    }

                    bool isAlias = this.DscExecutablePathIsAlias ?? false;
                    this.processorPathHandle = ProcessorPathIntegrity.VerifyAndOpen(
                        this.DscExecutablePath!,
                        this.DscExecutablePathHash,
                        isAlias);
                    this.processorPathVerified = true;
                }
            }
        }

        private ResourceDetails? TryGetFromCache(string resourceType)
        {
            lock (this.resourceDetailsDictionary)
            {
                this.resourceDetailsDictionary.TryGetValue(resourceType, out ResourceDetails? result);
                return result;
            }
        }

        private List<string> GetCachedAdapterTypes()
        {
            List<string> adapters = new List<string>();
            lock (this.resourceDetailsDictionary)
            {
                foreach (var kvp in this.resourceDetailsDictionary)
                {
                    if (kvp.Value.IsAdapter)
                    {
                        adapters.Add(kvp.Key);
                    }
                }
            }

            return adapters;
        }

        /// <summary>
        /// Populates the resource details cache from the provided list.
        /// Must be called while holding the resourceDetailsDictionary lock.
        /// </summary>
        private void PopulateCacheFromResourceListUnderLock(IList<IResourceListItem> items)
        {
            foreach (var item in items)
            {
                if (!this.resourceDetailsDictionary.ContainsKey(item.Type))
                {
                    var details = new ResourceDetails(item.Type);
                    details.SetResourceListItem(item);
                    this.resourceDetailsDictionary.Add(item.Type, details);
                }
            }
        }

        private void EnsureAllResourcesCached()
        {
            lock (this.resourceDetailsDictionary)
            {
                if (this.cacheState.AllResourcesCached)
                {
                    return;
                }
            }

            var resources = this.DSCv3.GetAllResources(null);

            lock (this.resourceDetailsDictionary)
            {
                if (!this.cacheState.AllResourcesCached)
                {
                    this.PopulateCacheFromResourceListUnderLock(resources);
                    this.cacheState.AllResourcesCached = true;
                }
            }
        }

        private void EnsureAdapterResourcesCached(string adapterType)
        {
            lock (this.resourceDetailsDictionary)
            {
                if (this.cacheState.CachedAdapters.Contains(adapterType))
                {
                    return;
                }
            }

            var resources = this.DSCv3.GetAllResourcesFromAdapter(adapterType, null);

            lock (this.resourceDetailsDictionary)
            {
                if (!this.cacheState.CachedAdapters.Contains(adapterType))
                {
                    this.PopulateCacheFromResourceListUnderLock(resources);
                    this.cacheState.CachedAdapters.Add(adapterType);
                }
            }
        }

        /// <summary>
        /// Tracks whether bulk resource listing calls have been made, shared between cloned instances.
        /// </summary>
        private class ResourceCacheState
        {
            /// <summary>
            /// Gets or sets a value indicating whether all top-level resources have been fetched and cached.
            /// </summary>
            public bool AllResourcesCached { get; set; } = false;

            /// <summary>
            /// Gets the set of adapter type names whose resources have been fully fetched and cached.
            /// </summary>
            public HashSet<string> CachedAdapters { get; } = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        }
    }
}
