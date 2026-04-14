#include <kotuku/main.h>
#include <kotuku/strings.hpp>
#include <iostream>
#include <string>

struct TestContext {
   int total_checks{0};
   int failed_checks{0};

   void expect_true(bool Condition, char const *Message) {
      total_checks += 1;
      if (not Condition) {
         failed_checks += 1;
         std::cout << "FAILED: " << Message << '\n';
      }
   }

   void expect_false(bool Condition, char const *Message) {
      total_checks += 1;
      if (Condition) {
         failed_checks += 1;
         std::cout << "FAILED: " << Message << '\n';
      }
   }

   void summary() const {
      if (failed_checks IS 0) {
         std::cout << "All " << total_checks << " checks passed." << '\n';
      } else {
         std::cout << failed_checks << " of " << total_checks << " checks failed." << '\n';
      }
   }
};

void test_basic_matching(TestContext &Context) {
   Context.expect_true(pf::wildcmp("hello", "hello"), "Exact match returns true");
   Context.expect_false(pf::wildcmp("hello", "world"), "Different strings return false");
   Context.expect_true(pf::wildcmp("", "anything"), "Empty wildcard matches anything");
   Context.expect_false(pf::wildcmp("hello", ""), "Non-empty wildcard does not match empty string");
}

void test_asterisk_wildcard(TestContext &Context) {
   Context.expect_true(pf::wildcmp("*", "anything"), "Single asterisk matches any string");
   Context.expect_true(pf::wildcmp("*", ""), "Single asterisk matches empty string");
   Context.expect_true(pf::wildcmp("hello*", "hello"), "Trailing asterisk matches exact prefix");
   Context.expect_true(pf::wildcmp("hello*", "helloworld"), "Trailing asterisk matches with suffix");
   Context.expect_true(pf::wildcmp("*world", "world"), "Leading asterisk matches exact suffix");
   Context.expect_true(pf::wildcmp("*world", "helloworld"), "Leading asterisk matches with prefix");
   Context.expect_true(pf::wildcmp("*test*", "test"), "Asterisks on both sides match exact string");
   Context.expect_true(pf::wildcmp("*test*", "pretestpost"), "Asterisks on both sides match with prefix and suffix");
   Context.expect_true(pf::wildcmp("**hello**", "hello"), "Multiple asterisks work like single asterisk");
   Context.expect_false(pf::wildcmp("hello*", "help"), "Trailing asterisk does not match different prefix");
   Context.expect_false(pf::wildcmp("*world", "word"), "Leading asterisk does not match different suffix");
}

void test_question_mark_wildcard(TestContext &Context) {
   Context.expect_true(pf::wildcmp("h?llo", "hello"), "Question mark matches single character");
   Context.expect_true(pf::wildcmp("h?llo", "hallo"), "Question mark matches different single character");
   Context.expect_false(pf::wildcmp("h?llo", "hllo"), "Question mark does not match missing character");
   Context.expect_false(pf::wildcmp("h?llo", "heello"), "Question mark does not match multiple characters");
   Context.expect_true(pf::wildcmp("???", "abc"), "Multiple question marks match equal length string");
   Context.expect_false(pf::wildcmp("???", "ab"), "Multiple question marks do not match shorter string");
   Context.expect_false(pf::wildcmp("???", "abcd"), "Multiple question marks do not match longer string");
}

void test_mixed_wildcards(TestContext &Context) {
   Context.expect_true(pf::wildcmp("?*", "a"), "Question mark followed by asterisk matches single character");
   Context.expect_true(pf::wildcmp("?*", "hello"), "Question mark followed by asterisk matches longer string");
   Context.expect_false(pf::wildcmp("?*", ""), "Question mark followed by asterisk does not match empty string");
   Context.expect_true(pf::wildcmp("h*o", "hello"), "Asterisk in middle matches characters");
   Context.expect_true(pf::wildcmp("h*o", "ho"), "Asterisk in middle matches no characters");
   Context.expect_true(pf::wildcmp("*h?llo*", "hello"), "Complex pattern with asterisks and question mark");
}

void test_or_operator(TestContext &Context) {
   Context.expect_true(pf::wildcmp("hello|world", "hello"), "OR operator matches first alternative");
   Context.expect_true(pf::wildcmp("hello|world", "world"), "OR operator matches second alternative");
   Context.expect_false(pf::wildcmp("hello|world", "test"), "OR operator does not match non-alternatives");
   Context.expect_true(pf::wildcmp("a|b|c", "b"), "Multiple OR alternatives work");
   Context.expect_true(pf::wildcmp("test*|*world", "testing"), "OR with wildcards in first alternative");
   Context.expect_true(pf::wildcmp("test*|*world", "helloworld"), "OR with wildcards in second alternative");
   Context.expect_true(pf::wildcmp("h?llo|w?rld", "hello"), "OR with question marks in first alternative");
   Context.expect_true(pf::wildcmp("h?llo|w?rld", "world"), "OR with question marks in second alternative");
}

void test_escape_sequences(TestContext &Context) {
   Context.expect_true(pf::wildcmp("hello\\*", "hello*"), "Escaped asterisk matches literal asterisk");
   Context.expect_false(pf::wildcmp("hello\\*", "helloworld"), "Escaped asterisk does not act as wildcard");
   Context.expect_true(pf::wildcmp("hello\\?", "hello?"), "Escaped question mark matches literal question mark");
   Context.expect_false(pf::wildcmp("hello\\?", "hellox"), "Escaped question mark does not act as wildcard");
   Context.expect_true(pf::wildcmp("hello\\|world", "hello|world"), "Escaped pipe matches literal pipe");
   Context.expect_false(pf::wildcmp("hello\\|world", "hello"), "Escaped pipe does not act as OR operator");
   Context.expect_true(pf::wildcmp("test\\\\", "test\\"), "Escaped backslash matches literal backslash");
}

void test_case_sensitivity(TestContext &Context) {
   Context.expect_true(pf::wildcmp("hello", "hello", false), "Case insensitive exact match");
   Context.expect_true(pf::wildcmp("hello", "HELLO", false), "Case insensitive different case match");
   Context.expect_true(pf::wildcmp("HELLO", "hello", false), "Case insensitive reverse case match");
   Context.expect_true(pf::wildcmp("h*o", "HELLO", false), "Case insensitive wildcard match");
   Context.expect_false(pf::wildcmp("hello", "HELLO", true), "Case sensitive different case no match");
   Context.expect_true(pf::wildcmp("hello", "hello", true), "Case sensitive same case match");
   Context.expect_true(pf::wildcmp("H?LLO", "hello", false), "Case insensitive question mark match");
   Context.expect_false(pf::wildcmp("H?LLO", "hello", true), "Case sensitive question mark no match");
}

void test_special_cases_with_or(TestContext &Context) {
   Context.expect_true(pf::wildcmp("*.txt", "file.txt", false), "Asterisk followed by pipe-terminated pattern");
   Context.expect_true(pf::wildcmp("test*|", "test123"), "OR with empty second alternative matches first");
   Context.expect_true(pf::wildcmp("|test", "test"), "OR with empty first alternative matches second");
   Context.expect_true(pf::wildcmp("fail|*", "anything"), "OR fallback to wildcard matches anything");
}

void test_edge_cases(TestContext &Context) {
   Context.expect_true(pf::wildcmp("", ""), "Empty wildcard matches empty string");
   Context.expect_true(pf::wildcmp("*", ""), "Asterisk wildcard matches empty string");
   Context.expect_false(pf::wildcmp("?", ""), "Question mark does not match empty string");
   Context.expect_true(pf::wildcmp("a*a", "aa"), "Asterisk between same characters matches minimal");
   Context.expect_true(pf::wildcmp("a*a", "aba"), "Asterisk between same characters matches with middle");
   Context.expect_true(pf::wildcmp("a*a", "abba"), "Asterisk between same characters matches multiple middle");
   Context.expect_false(pf::wildcmp("a*b", "a"), "Pattern requiring ending character must have that character");
   Context.expect_true(pf::wildcmp("***", "anything"), "Multiple consecutive asterisks work");
   Context.expect_true(pf::wildcmp("???***", "abc"), "Question marks followed by asterisks");
}

void test_complex_patterns(TestContext &Context) {
   Context.expect_true(pf::wildcmp("*.txt|*.doc", "file.txt"), "File extension pattern with OR");
   Context.expect_true(pf::wildcmp("*.txt|*.doc", "document.doc"), "File extension pattern with OR second match");
   Context.expect_false(pf::wildcmp("*.txt|*.doc", "file.pdf"), "File extension pattern with OR no match");
   Context.expect_true(pf::wildcmp("test_??.log|error_*.txt", "test_01.log"), "Complex pattern first alternative");
   Context.expect_true(pf::wildcmp("test_??.log|error_*.txt", "error_fatal.txt"), "Complex pattern second alternative");
   Context.expect_true(pf::wildcmp("*hello*world*", "say hello beautiful world today"), "Multiple asterisks with required substrings");
   Context.expect_false(pf::wildcmp("*hello*world*", "say hello beautiful earth today"), "Multiple asterisks missing required substring");
}

int main() {
   TestContext test_context;
   test_basic_matching(test_context);
   test_asterisk_wildcard(test_context);
   test_question_mark_wildcard(test_context);
   test_mixed_wildcards(test_context);
   test_or_operator(test_context);
   test_escape_sequences(test_context);
   test_case_sensitivity(test_context);
   test_special_cases_with_or(test_context);
   test_edge_cases(test_context);
   test_complex_patterns(test_context);
   test_context.summary();
   return test_context.failed_checks IS 0 ? 0 : 1;
}
