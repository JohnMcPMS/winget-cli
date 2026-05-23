// -----------------------------------------------------------------------------
// <copyright file="ResourceCache.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace Microsoft.Management.Configuration.Processor.DSCv3.Helpers
{
    using System;
    using System.Collections.Generic;
    using Microsoft.Management.Configuration.Processor.DSCv3.Model;
    using Microsoft.Management.Configuration.Processor.Helpers;

    /// <summary>
    /// Manages the resource details cache shared across cloned <see cref="ProcessorSettings"/> instances.
    /// </summary>
    internal class ResourceCache
    {
        private readonly Dictionary<string, ResourceDetails> resourceDetailsDictionary = new (StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> cachedAdapters = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private bool allResourcesCached = false;

        /// <summary>
        /// Gets the ResourceDetails for a configuration unit.
        /// </summary>
        /// <param name="configurationUnitInternal">The configuration unit to find details for.</param>
        /// <param name="detailFlags">The level of detail to get.</param>
        /// <param name="processorSettings">The processor settings to use when fetching details.</param>
        /// <returns>The ResourceDetails for the unit, or null if not found.</returns>
        public ResourceDetails? GetResourceDetails(ConfigurationUnitInternal configurationUnitInternal, ConfigurationUnitDetailFlags detailFlags, ProcessorSettings processorSettings)
        {
            // Check cache first.
            ResourceDetails? result = this.TryGetFromCache(configurationUnitInternal.QualifiedName);

            if (result == null)
            {
                // Prepopulate cache with all top-level resources if not done yet.
                this.EnsureAllResourcesCached(processorSettings.DSCv3);
                result = this.TryGetFromCache(configurationUnitInternal.QualifiedName);
            }

            if (result == null)
            {
                // Not a top-level resource; check within each known adapter.
                List<string> adapterTypes = this.GetCachedAdapterTypes();

                foreach (string adapterType in adapterTypes)
                {
                    this.EnsureAdapterResourcesCached(adapterType, processorSettings.DSCv3);
                    result = this.TryGetFromCache(configurationUnitInternal.QualifiedName);
                    if (result != null)
                    {
                        break;
                    }
                }
            }

            if (result != null)
            {
                result.EnsureDetails(processorSettings, detailFlags);

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
        /// <param name="processorSettings">The processor settings to use when fetching details.</param>
        /// <returns>A list of ResourceDetails.</returns>
        public List<ResourceDetails> FindAllResourceDetails(FindUnitProcessorsOptions findOptions, ProcessorSettings processorSettings)
        {
            List<ResourceDetails> result = new List<ResourceDetails>();

            var runSettings = ProcessorRunSettings.CreateFromFindUnitProcessorsOptions(findOptions);
            var resourceItemList = processorSettings.DSCv3.GetAllResources(runSettings);

            lock (this.resourceDetailsDictionary)
            {
                this.PopulateCacheFromResourceListUnderLock(resourceItemList);

                // If no custom search paths were used, the result represents the full default resource set.
                if (string.IsNullOrEmpty(findOptions.SearchPaths))
                {
                    this.allResourcesCached = true;
                }
            }

            foreach (var item in resourceItemList)
            {
                ResourceDetails? details = this.TryGetFromCache(item.Type);

                if (details != null)
                {
                    details.EnsureDetails(processorSettings, findOptions.UnitDetailFlags);
                    result.Add(details);
                }
            }

            return result;
        }

        /// <summary>
        /// Resets the cached state flags so that subsequent lookups will re-fetch from DSC.
        /// Existing <see cref="ResourceDetails"/> entries remain valid; only the
        /// "fully populated" markers are cleared so newly introduced resources can be discovered.
        /// </summary>
        public void IndicatePotentialResourceChanges()
        {
            lock (this.resourceDetailsDictionary)
            {
                this.allResourcesCached = false;
                this.cachedAdapters.Clear();
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
                foreach (var item in this.resourceDetailsDictionary)
                {
                    if (item.Value.IsAdapter)
                    {
                        adapters.Add(item.Key);
                    }
                }
            }

            return adapters;
        }

        /// <summary>
        /// Populates the resource details cache from the provided list.
        /// Existing entries are replaced so that changed resource metadata is always current.
        /// Must be called while holding the resourceDetailsDictionary lock.
        /// </summary>
        private void PopulateCacheFromResourceListUnderLock(IList<IResourceListItem> items)
        {
            foreach (var item in items)
            {
                var details = new ResourceDetails(item.Type);
                details.SetResourceListItem(item);
                this.resourceDetailsDictionary[item.Type] = details;
            }
        }

        private void EnsureAllResourcesCached(IDSCv3 dsc)
        {
            lock (this.resourceDetailsDictionary)
            {
                if (this.allResourcesCached)
                {
                    return;
                }
            }

            var resources = dsc.GetAllResources(null);

            lock (this.resourceDetailsDictionary)
            {
                if (!this.allResourcesCached)
                {
                    this.PopulateCacheFromResourceListUnderLock(resources);
                    this.allResourcesCached = true;
                }
            }
        }

        private void EnsureAdapterResourcesCached(string adapterType, IDSCv3 dsc)
        {
            lock (this.resourceDetailsDictionary)
            {
                if (this.cachedAdapters.Contains(adapterType))
                {
                    return;
                }
            }

            var resources = dsc.GetAllResourcesFromAdapter(adapterType, null);

            lock (this.resourceDetailsDictionary)
            {
                if (!this.cachedAdapters.Contains(adapterType))
                {
                    this.PopulateCacheFromResourceListUnderLock(resources);
                    this.cachedAdapters.Add(adapterType);
                }
            }
        }
    }
}
