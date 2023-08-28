// -----------------------------------------------------------------------------
// <copyright file="ConfigurationExtensions.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace Microsoft.Management.Configuration.UnitTests.Helpers
{
    using System;
    using System.Collections.Generic;
    using System.Linq;
    using System.Reflection;

    /// <summary>
    /// Contains extension methods for configuration objects.
    /// </summary>
    internal static class ConfigurationExtensions
    {
        /// <summary>
        /// Assigns the given properties to the configuration unit.
        /// </summary>
        /// <param name="unit">The unit to assign the properties of.</param>
        /// <param name="properties">The properties to assign.</param>
        /// <returns>The given ConfigurationUnit.</returns>
        internal static ConfigurationUnit Assign(this ConfigurationUnit unit, object properties)
        {
            PropertyInfo[] unitProperties = typeof(ConfigurationUnit).GetProperties();

            foreach (PropertyInfo property in properties.GetType().GetProperties())
            {
                var propertyValue = property.GetValue(properties);

                switch (property.Name)
                {
                    case "Units":
                        unit.Units((IEnumerable<ConfigurationUnit>?)propertyValue);
                        break;
                    case "Dependencies":
                        unit.Dependencies((IEnumerable<string>?)propertyValue);
                        break;
                    default:
                        PropertyInfo? matchingProperty = unitProperties.FirstOrDefault(pi => pi.Name == property.Name);
                        if (matchingProperty != null)
                        {
                            matchingProperty.SetValue(unit, property.GetValue(properties));
                        }
                        else
                        {
                            throw new ArgumentException($"ConfigurationUnit does not have property: {property.Name}");
                        }
                        break;
                }

            }

            return unit;
        }

        /// <summary>
        /// Assigns the given units to the configuration set.
        /// </summary>
        /// <param name="set">The set to assign the units of.</param>
        /// <param name="values">The units to assign.</param>
        internal static void Units(this ConfigurationSet set, IEnumerable<ConfigurationUnit> values)
        {
            AssignEnumerableToList(set.Units, values);
        }

        /// <summary>
        /// Assigns the given units to the configuration unit.
        /// </summary>
        /// <param name="unit">The unit to assign the units of.</param>
        /// <param name="values">The units to assign.</param>
        internal static void Units(this ConfigurationUnit unit, IEnumerable<ConfigurationUnit>? values)
        {
            if (values == null)
            {
                unit.IsGroup = false;
            }
            else
            {
                unit.IsGroup = true;
                AssignEnumerableToList(unit.Units, values);
            }
        }

        /// <summary>
        /// Assigns the given strings to the unit's dependencies.
        /// </summary>
        /// <param name="unit">The unit to assign the dependencies of.</param>
        /// <param name="values">The values to assign.</param>
        internal static void Dependencies(this ConfigurationUnit unit, IEnumerable<string>? values)
        {
            AssignEnumerableToList(unit.Dependencies, values);
        }

        private static void AssignEnumerableToList<T>(IList<T> target, IEnumerable<T>? values)
        {
            target.Clear();
            if (values != null)
            {
                foreach (var value in values)
                {
                    target.Add(value);
                }
            }
        }
    }
}
