// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace AppInstaller::Utility
{
    // Converts the given UTF16 string to UTF8
    std::string ConvertToUTF8(std::wstring_view input);

    // Converts the given UTF8 string to UTF16
    std::wstring ConvertToUTF16(std::string_view input, UINT codePage = CP_UTF8);

    // Converts the given UTF8 string to UTF32
    std::u32string ConvertToUTF32(std::string_view input);

    template <typename C, size_t Size>
    std::basic_string_view<C> StringViewFromLiteral(const C(&s)[Size])
    {
        return std::basic_string_view<C>{ s, (s[Size - 1] == '\0' ? Size - 1 : Size) };
    }

    template <typename C>
    struct CharacterTraits
    {
        // static constexpr size_t index; contains tuple index
    };

    template<>
    struct CharacterTraits<char>
    {
        static constexpr size_t index = 0;

        static std::string_view ConvertFrom(std::string_view s);
        static std::string ConvertFrom(std::wstring_view s);
    };

    template<>
    struct CharacterTraits<wchar_t>
    {
        static constexpr size_t index = 1;

        static std::wstring ConvertFrom(std::string_view s);
        static std::wstring_view ConvertFrom(std::wstring_view s);
    };

    // A string-like container that caches the value in various unicode transformation formats.
    struct UTFString
    {
        using StorageType = std::tuple<std::string, std::wstring>;

        UTFString() = default;

        UTFString(const UTFString&) = default;
        UTFString& operator=(const UTFString&) = default;

        UTFString(UTFString&&) = default;
        UTFString& operator=(UTFString&&) = default;

        // Constructors from input
        template <typename C, size_t Size>
        explicit UTFString(const C(&s)[Size]) { SetPrimary<C>(std::basic_string<C>{ StringViewFromLiteral(s) }); }

        template <typename C>
        explicit UTFString(std::basic_string_view<C> sv) { SetPrimary<C>(std::basic_string<C>{ sv }); }

        template <typename C>
        explicit UTFString(std::basic_string<C>& s) { SetPrimary<C>(s); }

        template <typename C>
        explicit UTFString(const std::basic_string<C>& s) { SetPrimary<C>(s); }

        template <typename C>
        explicit UTFString(std::basic_string<C>&& s) noexcept { SetPrimary<C>(std::move(s)); }

        // Assignment operators
        template <typename C, size_t Size>
        UTFString& operator=(const C(&s)[Size])
        {
            ClearAndSetPrimary<C>(std::basic_string<C>{ StringViewFromLiteral(s) });
            return *this;
        }

        template <typename C>
        UTFString& operator=(std::basic_string_view<C> sv)
        {
            ClearAndSetPrimary<C>(std::basic_string<C>{ sv });
            return *this;
        }

        template <typename C>
        UTFString& operator=(std::basic_string<C>& s)
        {
            ClearAndSetPrimary<C>(s);
            return *this;
        }

        template <typename C>
        UTFString& operator=(const std::basic_string<C>& s)
        {
            ClearAndSetPrimary<C>(s);
            return *this;
        }

        template <typename C>
        UTFString& operator=(std::basic_string<C>&& s)
        {
            ClearAndSetPrimary<C>(std::move(s));
            return *this;
        }

        void clear() noexcept;
        bool empty() const noexcept;

        template <typename C>
        const std::basic_string<C>& get() const
        {
            // If the value is not empty, it is already converted
            const std::basic_string<C>& cachedResult = Get<C>();
            if (CharacterTraits<C>::index == m_primary || !cachedResult.empty())
            {
                return cachedResult;
            }

            return ConvertTo<C>();
        }

        // By default use the UTF-8 string
        const std::string& get() const { return get<char>(); }

        const std::string* operator->() const { return &get<char>(); }

        template <typename C>
        operator const std::basic_string<C>& () const
        {
            return get<C>();
        }

        template <typename C>
        operator std::basic_string_view<C> () const
        {
            return get<C>();
        }

        template <typename C>
        const C* c_str() const
        {
            return get<C>().c_str();
        }

        // Comparison operators
        template <typename C, size_t Size>
        bool operator==(const C(&s)[Size]) const { return get<C>() == StringViewFromLiteral(s); }

        template <typename C>
        bool operator==(std::basic_string_view<C> sv) const { return get<C>() == sv; }

        template <typename C>
        bool operator==(std::basic_string<C>& s) const { return get<C>() == s; }

        template <typename C>
        bool operator==(const std::basic_string<C>& s) const { return get<C>() == s; }

        template <typename S>
        bool operator!=(S&& s) const { return !(operator==(std::forward<S>(s))); }

        bool operator==(const UTFString& other) const;
        bool operator!=(const UTFString& other) const { return !(operator==(other)); }
        bool operator<(const UTFString& other) const;

        // Construction operators
        template <typename S>
        auto operator+(S&& s) const { return get() + std::forward<S>(s); }

    protected:
        template <typename C>
        void SetSecondary(const std::basic_string<C>& s)
        {
            Get<C>() = s;
        }

        template <typename C>
        void SetSecondary(std::basic_string<C>&& s)
        {
            Get<C>() = std::move(s);
        }

        template <typename C, typename S>
        void SetPrimary(S&& s)
        {
            m_primary = CharacterTraits<C>::index;
            SetSecondary<C>(std::forward<S>(s));
        }

        template <typename C, typename S>
        void ClearAndSetPrimary(S&& s)
        {
            clear();
            SetPrimary<C>(std::forward<S>(s));
        }

        template <typename C>
        const std::basic_string<C>& Get() const
        {
            return std::get<CharacterTraits<C>::index>(m_storage);
        }

        template <typename C>
        std::basic_string<C>& Get()
        {
            return std::get<CharacterTraits<C>::index>(m_storage);
        }

    private:
        template <typename C, std::size_t... Indices>
        std::basic_string<C> ConvertToHelper(C, std::index_sequence<Indices...>) const
        {
            std::basic_string<C> result;
            ((Indices == m_primary ? void(result = CharacterTraits<C>::ConvertFrom(std::get<Indices>(m_storage))) : void()), ...);
            return result;
        }

        template <typename C>
        const std::basic_string<C>& ConvertTo() const
        {
            // Despite being const, store the converted value
            std::basic_string<C>& result = const_cast<UTFString*>(this)->Get<C>();
            result = ConvertToHelper(C{}, std::make_index_sequence<std::tuple_size_v<StorageType>>{});
            return result;
        }

        static constexpr size_t NotSet = std::numeric_limits<size_t>::max();
        size_t m_primary = NotSet;

        StorageType m_storage;
    };

    template <typename S>
    bool operator==(S&& s, const UTFString& string) { return string.operator==(std::forward<S>(s)); }

    template <typename S>
    bool operator!=(S&& s, const UTFString& string) { return !(string.operator==(std::forward<S>(s))); }

    template <typename S>
    auto operator+(S&& s, const UTFString& string) { return std::forward<S>(s) + string.get(); }

    std::ostream& operator<<(std::ostream& out, const UTFString& string) { return (out << string.get()); }

    // Normalizes a UTF8 string to the given form.
    std::string Normalize(std::string_view input, NORM_FORM form = NORM_FORM::NormalizationKC);

    // Normalizes a UTF16 string to the given form.
    std::wstring Normalize(std::wstring_view input, NORM_FORM form = NORM_FORM::NormalizationKC);

    // Type to hold and force a normalized UTF string.
    template <NORM_FORM Form = NORM_FORM::NormalizationKC>
    struct NormalizedUTF : public UTFString
    {
        // Indicates that the incoming value is already normalized.
        struct PreNormalized_t {};

        NormalizedUTF() = default;

        NormalizedUTF(const NormalizedUTF& other) = default;
        NormalizedUTF& operator=(const NormalizedUTF& other) = default;

        template <NORM_FORM OtherForm>
        NormalizedUTF(const NormalizedUTF<OtherForm>& other) : NormalizedUTF(other.get<wchar_t>()) {}

        template <NORM_FORM OtherForm>
        NormalizedUTF& operator=(const NormalizedUTF<OtherForm>& other)
        {
            ClearAndSetNormalizedPrimary(other.get<wchar_t>());
            return *this;
        }

        NormalizedUTF(NormalizedUTF&& other) = default;
        NormalizedUTF& operator=(NormalizedUTF&& other) = default;

        // Constructors from input
        template <typename C, size_t Size>
        explicit NormalizedUTF(const C(&s)[Size]) { SetNormalizedPrimary(StringViewFromLiteral(s)); }

        template <typename C>
        explicit NormalizedUTF(std::basic_string_view<C> sv) { SetNormalizedPrimary(sv); }

        template <typename C>
        explicit NormalizedUTF(std::basic_string<C>& s) { SetNormalizedPrimary(s); }

        template <typename C>
        explicit NormalizedUTF(const std::basic_string<C>& s) { SetNormalizedPrimary(s); }

        template <typename C>
        explicit NormalizedUTF(std::basic_string<C>&& s) noexcept { SetNormalizedPrimary(std::move(s)); }

        template <typename C>
        explicit NormalizedUTF(std::basic_string<C>&& s, PreNormalized_t) noexcept { SetPrimary<C>(std::move(s)); }

        // Assignment operators
        template <typename C, size_t Size>
        NormalizedUTF& operator=(const C(&s)[Size])
        {
            ClearAndSetNormalizedPrimary(StringViewFromLiteral(s));
            return *this;
        }

        template <typename C>
        NormalizedUTF& operator=(std::basic_string_view<C> sv)
        {
            ClearAndSetNormalizedPrimary(sv);
            return *this;
        }

        template <typename C>
        NormalizedUTF& operator=(std::basic_string<C>& s)
        {
            ClearAndSetNormalizedPrimary(s);
            return *this;
        }

        template <typename C>
        NormalizedUTF& operator=(const std::basic_string<C>& s)
        {
            ClearAndSetNormalizedPrimary(s);
            return *this;
        }

        template <typename C>
        NormalizedUTF& operator=(std::basic_string<C>&& s)
        {
            ClearAndSetNormalizedPrimary(std::move(s));
            return *this;
        }

    protected:
        // Store wchar_t strings as primary because that is the native string type for the normalization function.
        template <typename S>
        void SetNormalizedPrimary(S&& s)
        {
            UTFString::SetPrimary<wchar_t>(Normalize(CharacterTraits<wchar_t>::ConvertFrom(std::forward<S>(s)), Form));
        }

        template <typename S>
        void ClearAndSetNormalizedPrimary(S&& s)
        {
            UTFString::ClearAndSetPrimary<wchar_t>(Normalize(CharacterTraits<wchar_t>::ConvertFrom(std::forward<S>(s)), Form));
        }
    };

    using NormalizedString = NormalizedUTF<>;

    // Compares the two UTF8 strings in a case insensitive manner.
    // Use this if one of the values is a known value, and thus ToLower is sufficient.
    bool CaseInsensitiveEquals(std::string_view a, std::string_view b);

    // Determines if string a starts with string b.
    // Use this if one of the values is a known value, and thus ToLower is sufficient.
    bool CaseInsensitiveStartsWith(std::string_view a, std::string_view b);

    // Compares the two UTF8 strings in a case insensitive manner, using ICU for case folding.
    bool ICUCaseInsensitiveEquals(std::string_view a, std::string_view b);

    // Determines if string a starts with string b, using ICU for case folding.
    bool ICUCaseInsensitiveStartsWith(std::string_view a, std::string_view b);

    // Returns the number of grapheme clusters (characters) in an UTF8-encoded string.
    size_t UTF8Length(std::string_view input);

    // Returns the number of units the UTF8-encoded string will take in terminal output. Some characters take 2 units in terminal output.
    size_t UTF8ColumnWidth(const NormalizedUTF<NormalizationC>& input);

    // Returns a substring view in an UTF8-encoded string. Offset and count are measured in grapheme clusters (characters).
    std::string_view UTF8Substring(std::string_view input, size_t offset, size_t count);

    // Returns a substring view in an UTF8-encoded string trimmed to be at most expected length. Length is measured as units taken in terminal output.
    // Note the returned substring view might be less than specified length as some characters might take 2 units in terminal output.
    std::string UTF8TrimRightToColumnWidth(const NormalizedUTF<NormalizationC>&, size_t expectedWidth, size_t& actualWidth);

    // Get the lower case version of the given std::string
    std::string ToLower(std::string_view in);

    // Get the lower case version of the given std::wstring
    std::wstring ToLower(std::wstring_view in);

    // Folds the case of the given std::string
    // See https://unicode-org.github.io/icu/userguide/transforms/casemappings.html#case-folding
    std::string FoldCase(std::string_view input);

    // Folds the case of the given NormalizedString, returning it as also Normalized
    // See https://unicode-org.github.io/icu/userguide/transforms/casemappings.html#case-folding
    NormalizedString FoldCase(const NormalizedString& input);

    // Checks if the input string is empty or whitespace
    bool IsEmptyOrWhitespace(std::string_view str);
    bool IsEmptyOrWhitespace(std::wstring_view str);

    // Find token in the input string and replace with value.
    // Returns a value indicating whether a replacement occurred.
    bool FindAndReplace(std::string& inputStr, std::string_view token, std::string_view value);

    // Replaces the token in the input string with value while copying to a new result.
    std::wstring ReplaceWhileCopying(std::wstring_view input, std::wstring_view token, std::wstring_view value);

    // Removes whitespace from the beginning and end of the string.
    std::string& Trim(std::string& str);
    std::string Trim(std::string&& str);

    // Removes whitespace from the beginning and end of the string.
    std::wstring& Trim(std::wstring& str);

    // Reads the entire stream into a string.
    std::string ReadEntireStream(std::istream& stream);

    // Expands environment variables within the input.
    std::wstring ExpandEnvironmentVariables(const std::wstring& input);

    // Replace message predefined token
    std::string FindAndReplaceMessageToken(std::string_view message, std::string_view value);

    // Converts the candidate path part into one suitable for the actual file system
    std::string MakeSuitablePathPart(std::string_view candidate);

    // Gets the file name part of the given URI.
    std::filesystem::path GetFileNameFromURI(std::string_view uri);

    // Splits the string into words.
    std::vector<std::string> SplitIntoWords(std::string_view input);

    // Converts a container to a string representation of it.
    template <typename T, typename U>
    std::string ConvertContainerToString(const T& container, U toString)
    {
        std::ostringstream strstr;
        strstr << '[';

        bool firstItem = true;
        for (const auto& item : container)
        {
            if (firstItem)
            {
                firstItem = false;
            }
            else
            {
                strstr << ", ";
            }

            strstr << toString(item);
        }

        strstr << ']';
        return strstr.str();  // We need C++20 to get std::move(strstr).str() to extract the string from inside the stream
    }

    template <typename T>
    std::string ConvertContainerToString(const T& container)
    {
        return ConvertContainerToString(container, [](const auto& item) { return item; });
    }

    template <typename CharType>
    std::basic_string<CharType> StringOrEmptyIfNull(const CharType* string)
    {
        if (string)
        {
            return { string };
        }
        else
        {
            return {};
        }
    }
}
