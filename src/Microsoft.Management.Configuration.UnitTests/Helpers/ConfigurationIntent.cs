// -----------------------------------------------------------------------------
// <copyright file="ConfigurationIntent.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace Microsoft.Management.Configuration.UnitTests.Helpers
{
    /// <summary>
    /// The intent of a configuration.
    /// </summary>
    internal enum ConfigurationIntent
    {
        /// <summary>
        /// Assert intent.
        /// </summary>
        Assert,

        /// <summary>
        /// Inform intent.
        /// </summary>
        Inform,

        /// <summary>
        /// Apply intent.
        /// </summary>
        Apply,
    }
}
