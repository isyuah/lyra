import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import lyraGrammar from "./src/grammars/lyra.mjs";

// GitHub Pages project site: https://isyuah.github.io/lyra/
export default defineConfig({
  site: "https://isyuah.github.io",
  base: "/lyra",
  integrations: [
    starlight({
      title: {
        "zh-CN": "Lyra 文档",
        en: "Lyra Docs",
      },
      description:
        "Lyra 是一门使用 LLVM AOT 的编译型语言。这里提供从第一段程序到可交付服务的教程。",
      defaultLocale: "root",
      locales: {
        root: { label: "简体中文", lang: "zh-CN" },
        en: { label: "English", lang: "en" },
      },
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/isyuah/lyra",
        },
      ],
      editLink: {
        baseUrl: "https://github.com/isyuah/lyra/edit/main/docs/",
      },
      tableOfContents: { minHeadingLevel: 2, maxHeadingLevel: 3 },
      customCss: ["./src/styles/custom.css"],
      expressiveCode: {
        shiki: {
          langs: [lyraGrammar],
          langAlias: { output: "txt" },
        },
      },
      sidebar: [
        {
          label: "教程",
          translations: { en: "Tutorial" },
          items: [
            {
              label: "教程总览",
              translations: { en: "Tutorial overview" },
              slug: "guide",
            },
            {
              label: "开始",
              translations: { en: "Getting started" },
              items: [
                {
                  label: "Lyra 是什么",
                  translations: { en: "What is Lyra" },
                  slug: "guide/what-is-lyra",
                },
                {
                  label: "第一个项目",
                  translations: { en: "Your first project" },
                  slug: "guide/first-project",
                },
              ],
            },
            {
              label: "语言基础",
              translations: { en: "Language basics" },
              items: [
                {
                  label: "表达式、值与控制流",
                  translations: { en: "Expressions, values, and control flow" },
                  slug: "guide/values-and-control-flow",
                },
                {
                  label: "数据建模",
                  translations: { en: "Data modeling" },
                  slug: "guide/data-modeling",
                },
                {
                  label: "函数、闭包与集合",
                  translations: { en: "Functions, closures, and collections" },
                  slug: "guide/functions-closures-collections",
                },
              ],
            },
            {
              label: "语言能力",
              translations: { en: "Language capabilities" },
              items: [
                {
                  label: "泛型、接口与转换",
                  translations: { en: "Generics, interfaces, and conversions" },
                  slug: "guide/generics-interfaces",
                },
                {
                  label: "错误与资源",
                  translations: { en: "Errors and resources" },
                  slug: "guide/errors-resources",
                },
                {
                  label: "异步与并发",
                  translations: { en: "Async and concurrency" },
                  slug: "guide/async-concurrency",
                },
              ],
            },
            {
              label: "工程与边界",
              translations: { en: "Engineering and boundaries" },
              items: [
                {
                  label: "模块、包与测试",
                  translations: { en: "Modules, packages, and tests" },
                  slug: "guide/modules-packages-testing",
                },
                {
                  label: "元编程与外部边界",
                  translations: { en: "Metaprogramming and FFI" },
                  slug: "guide/metaprogramming-ffi",
                },
              ],
            },
            {
              label: "实战",
              translations: { en: "Capstones" },
              items: [
                {
                  label: "JSONL 统计 CLI",
                  translations: { en: "JSONL stats CLI" },
                  slug: "guide/jsonlines-cli",
                },
                {
                  label: "HTTP/SQLite 用户服务",
                  translations: { en: "HTTP/SQLite user service" },
                  slug: "guide/http-sqlite-service",
                },
              ],
            },
          ],
        },
        {
          label: "示例与源码",
          translations: { en: "Examples and source" },
          items: [
            {
              label: "仓库示例",
              translations: { en: "Repository examples" },
              link: "https://github.com/isyuah/lyra/tree/main/examples",
            },
            {
              label: "标准库索引",
              translations: { en: "Standard library index" },
              link: "https://github.com/isyuah/lyra/blob/main/std/README.md",
            },
          ],
        },
      ],
    }),
  ],
});
