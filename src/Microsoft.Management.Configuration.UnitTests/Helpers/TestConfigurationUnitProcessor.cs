// -----------------------------------------------------------------------------
// <copyright file="TestConfigurationUnitProcessor.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace Microsoft.Management.Configuration.UnitTests.Helpers
{
    using System;
    using System.Collections.Generic;
    using Windows.Foundation;

    /// <summary>
    /// A test implementation of IConfigurationUnitProcessor and IConfigurationGroupProcessor.
    /// </summary>
    internal class TestConfigurationUnitProcessor : IConfigurationUnitProcessor, IConfigurationGroupProcessor
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="TestConfigurationUnitProcessor"/> class.
        /// </summary>
        /// <param name="unit">The unit.</param>
        internal TestConfigurationUnitProcessor(ConfigurationUnit unit)
        {
            this.Unit = unit;
        }

        /// <summary>
        /// The delegate for ApplySettings.
        /// </summary>
        /// <returns>The result.</returns>
        internal delegate IApplySettingsResult ApplySettingsDelegateType();

        /// <summary>
        /// The delegate for GetSettings.
        /// </summary>
        /// <returns>The result.</returns>
        internal delegate IGetSettingsResult GetSettingsDelegateType();

        /// <summary>
        /// The delegate for TestSettings.
        /// </summary>
        /// <returns>The result.</returns>
        internal delegate ITestSettingsResult TestSettingsDelegateType();

        /// <summary>
        /// Gets the configuration unit.
        /// </summary>
        public ConfigurationUnit Unit { get; private set; }

        /// <summary>
        /// Gets or sets the delegate object for ApplySettings.
        /// </summary>
        internal ApplySettingsDelegateType? ApplySettingsDelegate { get; set; }

        /// <summary>
        /// Gets the number of times ApplySettings is called.
        /// </summary>
        internal int ApplySettingsCalls { get; private set; } = 0;

        /// <summary>
        /// Gets or sets the delegate object for GetSettings.
        /// </summary>
        internal GetSettingsDelegateType? GetSettingsDelegate { get; set; }

        /// <summary>
        /// Gets the number of times GetSettings is called.
        /// </summary>
        internal int GetSettingsCalls { get; private set; } = 0;

        /// <summary>
        /// Gets or sets the delegate object for TestSettings.
        /// </summary>
        internal TestSettingsDelegateType? TestSettingsDelegate { get; set; }

        /// <summary>
        /// Gets the number of times TestSettings is called.
        /// </summary>
        internal int TestSettingsCalls { get; private set; } = 0;

        /// <summary>
        /// Calls the ApplySettingsDelegate if one is provided; returns success if not.
        /// </summary>
        /// <returns>The result.</returns>
        public IApplySettingsResult ApplySettings()
        {
            ++this.ApplySettingsCalls;
            if (this.ApplySettingsDelegate != null)
            {
                return this.ApplySettingsDelegate();
            }
            else
            {
                return new ApplySettingsResultInstance(this.Unit);
            }
        }

        /// <summary>
        /// Calls the GetSettingsDelegate if one is provided; returns success if not (with no settings values).
        /// </summary>
        /// <returns>The result.</returns>
        public IGetSettingsResult GetSettings()
        {
            ++this.GetSettingsCalls;
            if (this.GetSettingsDelegate != null)
            {
                return this.GetSettingsDelegate();
            }
            else
            {
                return new GetSettingsResultInstance(this.Unit);
            }
        }

        /// <summary>
        /// Calls the TestSettingsDelegate if one is provided; returns success if not (with a positive test result).
        /// </summary>
        /// <returns>The result.</returns>
        public ITestSettingsResult TestSettings()
        {
            ++this.TestSettingsCalls;
            if (this.TestSettingsDelegate != null)
            {
                return this.TestSettingsDelegate();
            }
            else
            {
                return new TestSettingsResultInstance(this.Unit) { TestResult = ConfigurationTestResult.Positive };
            }
        }

        /// <summary>
        /// Calls the ApplySettingsAsyncDelegate if one is provided; returns success for all members if not.
        /// </summary>
        /// <returns>An async operation.</returns>
        public IAsyncOperationWithProgress<IApplySettingsGroupResult, IApplySettingsResult> ApplySettingsAsync()
        {
            throw new NotImplementedException();
        }

        /// <summary>
        /// Calls the GetSettingsAsyncDelegate if one is provided; returns success for all members if not (with no settings values).
        /// </summary>
        /// <returns>An async operation.</returns>
        public IAsyncOperationWithProgress<IGetSettingsGroupResult, IGetSettingsResult> GetSettingsAsync()
        {
            throw new NotImplementedException();
        }

        /// <summary>
        /// Calls The TestSettingsAsyncDelegate if one is provided; returns success for all members if not (with a positive test result).
        /// </summary>
        /// <returns>An async operation.</returns>
        public IAsyncOperationWithProgress<ITestSettingsGroupResult, ITestSettingsResult> TestSettingsAsync()
        {
            throw new NotImplementedException();
        }
    }
}
