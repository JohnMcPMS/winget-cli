// -----------------------------------------------------------------------------
// <copyright file="ConfigurationUnitProcessorTests.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace Microsoft.Management.Configuration.UnitTests.Tests
{
    using System;
    using System.Collections.Generic;
    using System.Management.Automation;
    using Microsoft.Management.Configuration;
    using Microsoft.Management.Configuration.Processor.DscResourcesInfo;
    using Microsoft.Management.Configuration.Processor.Helpers;
    using Microsoft.Management.Configuration.Processor.ProcessorEnvironments;
    using Microsoft.Management.Configuration.Processor.Unit;
    using Microsoft.Management.Configuration.UnitTests.Fixtures;
    using Microsoft.PowerShell.Commands;
    using Moq;
    using Windows.Foundation.Collections;
    using Xunit;
    using Xunit.Abstractions;

    /// <summary>
    /// Configuration unit processor tests.
    /// </summary>
    [Collection("UnitTestCollection")]
    public class ConfigurationUnitProcessorTests
    {
        private readonly UnitTestFixture fixture;
        private readonly ITestOutputHelper log;

        /// <summary>
        /// Initializes a new instance of the <see cref="ConfigurationUnitProcessorTests"/> class.
        /// </summary>
        /// <param name="fixture">Unit test fixture.</param>
        /// <param name="log">Log helper.</param>
        public ConfigurationUnitProcessorTests(UnitTestFixture fixture, ITestOutputHelper log)
        {
            this.fixture = fixture;
            this.log = log;
        }

        /// <summary>
        /// Tests GetSettings when a System.Management.Automation.RuntimeException is thrown.
        /// </summary>
        [Fact]
        public void GetSettings_Throws_Pwsh_RuntimeException()
        {
            var thrownException = new RuntimeException("a message");
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeGetResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Throws(() => thrownException)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.GetSettings();

            processorEnvMock.Verify();

            // Do not check for the type.
            Assert.Equal(thrownException.HResult, result.ResultInformation.ResultCode.HResult);
            Assert.True(!string.IsNullOrWhiteSpace(result.ResultInformation.Description));
            Assert.Equal(ConfigurationUnitResultSource.Internal, result.ResultInformation.ResultSource);
        }

        /// <summary>
        /// Tests GetSettings when a Microsoft.PowerShell.Commands.WriteErrorException is thrown.
        /// </summary>
        [Fact]
        public void GetSettings_Throws_Pwsh_WriteErrorException()
        {
            var thrownException = new WriteErrorException("a message");
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeGetResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Throws(() => thrownException)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.GetSettings();

            processorEnvMock.Verify();

            // Do not check for the type.
            Assert.Equal(thrownException.HResult, result.ResultInformation.ResultCode.HResult);
            Assert.True(!string.IsNullOrWhiteSpace(result.ResultInformation.Description));
            Assert.Equal(ConfigurationUnitResultSource.Internal, result.ResultInformation.ResultSource);
        }

        /// <summary>
        /// Call TestSettings with Inform intent is not allowed.
        /// </summary>
        [Fact]
        public void TestSettings_InformIntent()
        {
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            Assert.Throws<NotSupportedException>(() => unitProcessor.TestSettings());
        }

        /// <summary>
        /// Tests TestSettings when a System.Management.Automation.RuntimeException is thrown.
        /// </summary>
        [Fact]
        public void TestSettings_Throws_Pwsh_RuntimeException()
        {
            var thrownException = new RuntimeException("a message");
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeTestResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Throws(() => thrownException)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.TestSettings();

            processorEnvMock.Verify();

            Assert.Equal(ConfigurationTestResult.Failed, result.TestResult);

            // Do not check for the type.
            Assert.Equal(thrownException.HResult, result.ResultInformation.ResultCode.HResult);
            Assert.True(!string.IsNullOrWhiteSpace(result.ResultInformation.Description));
            Assert.Equal(ConfigurationUnitResultSource.Internal, result.ResultInformation.ResultSource);
        }

        /// <summary>
        /// Tests TestSettings when a Microsoft.PowerShell.Commands.WriteErrorException is thrown.
        /// </summary>
        [Fact]
        public void TestSettings_Throws_Pwsh_WriteErrorException()
        {
            var thrownException = new WriteErrorException("a message");
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeTestResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Throws(() => thrownException)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.TestSettings();

            processorEnvMock.Verify();

            Assert.Equal(ConfigurationTestResult.Failed, result.TestResult);

            // Do not check for the type.
            Assert.Equal(thrownException.HResult, result.ResultInformation.ResultCode.HResult);
            Assert.True(!string.IsNullOrWhiteSpace(result.ResultInformation.Description));
            Assert.Equal(ConfigurationUnitResultSource.Internal, result.ResultInformation.ResultSource);
        }

        /// <summary>
        /// Call ApplySettings.
        /// </summary>
        /// <param name="rebootRequired">Reboot required.</param>
        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public void ApplySettings_Test(bool rebootRequired)
        {
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeSetResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Returns(rebootRequired)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.ApplySettings();

            Assert.Equal(rebootRequired, result.RebootRequired);
        }

        /// <summary>
        /// Tests ApplySettings when a System.Management.Automation.RuntimeException is thrown.
        /// </summary>
        [Fact]
        public void ApplySettings_Throws_Pwsh_RuntimeException()
        {
            var thrownException = new RuntimeException("a message");
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeSetResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Throws(() => thrownException)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.ApplySettings();

            processorEnvMock.Verify();

            // Do not check for the type.
            Assert.Equal(thrownException.HResult, result.ResultInformation.ResultCode.HResult);
            Assert.True(!string.IsNullOrWhiteSpace(result.ResultInformation.Description));
            Assert.Equal(ConfigurationUnitResultSource.Internal, result.ResultInformation.ResultSource);
        }

        /// <summary>
        /// Tests ApplySettings when a Microsoft.PowerShell.Commands.WriteErrorException is thrown.
        /// </summary>
        [Fact]
        public void ApplySettings_Throws_Pwsh_WriteErrorException()
        {
            var thrownException = new RuntimeException("a message");
            var processorEnvMock = new Mock<IProcessorEnvironment>();
            processorEnvMock.Setup(m => m.InvokeSetResource(
                It.IsAny<ValueSet>(),
                It.IsAny<string>(),
                It.IsAny<ModuleSpecification?>()))
                .Throws(() => thrownException)
                .Verifiable();

            var unitResource = this.CreateUnitResource();

            var unitProcessor = new ConfigurationUnitProcessor(processorEnvMock.Object, unitResource);

            var result = unitProcessor.ApplySettings();

            processorEnvMock.Verify();

            // Do not check for the type.
            Assert.Equal(thrownException.HResult, result.ResultInformation.ResultCode.HResult);
            Assert.True(!string.IsNullOrWhiteSpace(result.ResultInformation.Description));
            Assert.Equal(ConfigurationUnitResultSource.Internal, result.ResultInformation.ResultSource);
        }

        private ConfigurationUnitAndResource CreateUnitResource()
        {
            string resourceName = "xResourceName";
            return new ConfigurationUnitAndResource(
                new ConfigurationUnitInternal(
                    new ConfigurationUnit
                    {
                        Type = resourceName,
                    },
                    string.Empty),
                new DscResourceInfoInternal(resourceName, null, null));
        }
    }
}
