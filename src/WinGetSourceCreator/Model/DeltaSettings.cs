// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

namespace WinGetSourceCreator.Model
{
    // Describes a delta capable source to publish alongside the ordinary one.
    //
    // It is published as its own source rather than layered onto the existing packages so that a
    // client that knows nothing about deltas keeps seeing exactly what it saw before.
    public class DeltaSettings
    {
        // Directory, relative to the working directory, that the delta source is published under.
        // This is also what a client is pointed at, so everything the source offers lives below it.
        public string RelativeDirectory { get; set; } = "deltaSource";

        // Identity name carried by both the full index and the baseline. MSIX forbids publishing
        // different content under one identity and version, so they are separated by version
        // instead: a baseline is simply the full index as it stood when it was captured.
        public string IdentityName { get; set; } = string.Empty;

        // Identity name carried by the delta, which has to differ from the above so that a client
        // can acquire one without the other.
        public string DeltaIdentityName { get; set; } = string.Empty;

        // The version of the full index and of the delta that describes it.
        public string Version { get; set; } = string.Empty;

        // The baseline is published at an older version than the full index, so that a client
        // already holding the full index has to move backward to pick the baseline up. That is the
        // ordinary state of a real source, and it is worth having the tests live in it.
        public string BaselineVersion { get; set; } = string.Empty;

        // Where the baseline is published, relative to the source's own location. The delta records
        // this, so it is what a client will ask for.
        public string BaselineRelativePath { get; set; } = "baseline/source2.msix";

        // Manifest file names left out of the baseline. Without these the delta would be empty and
        // a test could not tell a merged result apart from either half read on its own.
        public List<string> ManifestsNotInBaseline { get; set; } = new();

        public void Validate()
        {
            if (string.IsNullOrEmpty(this.RelativeDirectory))
            {
                throw new ArgumentNullException(nameof(this.RelativeDirectory));
            }

            if (string.IsNullOrEmpty(this.IdentityName))
            {
                throw new ArgumentNullException(nameof(this.IdentityName));
            }

            if (string.IsNullOrEmpty(this.DeltaIdentityName))
            {
                throw new ArgumentNullException(nameof(this.DeltaIdentityName));
            }

            if (this.IdentityName == this.DeltaIdentityName)
            {
                throw new ArgumentException("The delta must have an identity of its own", nameof(this.DeltaIdentityName));
            }

            if (string.IsNullOrEmpty(this.Version))
            {
                throw new ArgumentNullException(nameof(this.Version));
            }

            if (string.IsNullOrEmpty(this.BaselineVersion))
            {
                throw new ArgumentNullException(nameof(this.BaselineVersion));
            }

            if (string.IsNullOrEmpty(this.BaselineRelativePath))
            {
                throw new ArgumentNullException(nameof(this.BaselineRelativePath));
            }

            if (this.ManifestsNotInBaseline.Count == 0)
            {
                throw new ArgumentException("An empty delta cannot demonstrate anything", nameof(this.ManifestsNotInBaseline));
            }
        }
    }
}
