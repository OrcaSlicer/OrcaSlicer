#!/usr/bin/env bun

declare global {
  var process: {
    env: Record<string, string | undefined>;
  };
}

interface GitHubIssue {
  number: number;
  title: string;
  state: string;
  state_reason?: string;
  user: { id: number };
  created_at: string;
  closed_at?: string;
  pull_request?: unknown;
}

interface GitHubComment {
  id: number;
  body: string;
  created_at: string;
  user: { type: string; id: number };
}

async function githubRequest<T>(endpoint: string, token: string, method: string = 'GET', body?: any): Promise<T> {
  const response = await fetch(`https://api.github.com${endpoint}`, {
    method,
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: "application/vnd.github.v3+json",
      "User-Agent": "backfill-duplicate-comments-script",
      ...(body && { "Content-Type": "application/json" }),
    },
    ...(body && { body: JSON.stringify(body) }),
  });

  if (!response.ok) {
    throw new Error(
      `GitHub API request failed: ${response.status} ${response.statusText}`
    );
  }

  return response.json();
}

async function triggerDedupeWorkflow(
  owner: string,
  repo: string,
  issueNumber: number,
  token: string,
  dryRun: boolean = true
): Promise<void> {
  if (dryRun) {
    console.log(`[DRY RUN] Would trigger dedupe workflow for issue #${issueNumber}`);
    return;
  }

  await githubRequest(
    `/repos/${owner}/${repo}/actions/workflows/dedupe-issues.yml/dispatches`,
    token,
    'POST',
    {
      ref: 'main',
      inputs: {
        issue_number: issueNumber.toString()
      }
    }
  );
}

async function backfillDuplicateComments(): Promise<void> {
  console.log("[DEBUG] Starting backfill duplicate comments script");

  const token = process.env.GITHUB_TOKEN;
  if (!token) {
    throw new Error(`GITHUB_TOKEN environment variable is required

Usage:
  GITHUB_TOKEN=your_token bun run scripts/backfill-duplicate-comments.ts

Environment Variables:
  GITHUB_TOKEN - GitHub personal access token with repo and actions permissions (required)
  DRY_RUN - Set to "false" to actually trigger workflows (default: true for safety)
  MIN_ISSUE_NUMBER - Only process issues with numbers at or above this value (default: 1)
  MAX_ISSUE_NUMBER - Only process issues with numbers less than this value (default: no limit)
  LIMIT - Stop after triggering this many workflows (default: 100)`);
  }
  console.log("[DEBUG] GitHub token found");

  const owner = "OrcaSlicer";
  const repo = "OrcaSlicer";
  const dryRun = process.env.DRY_RUN !== "false";
  const maxIssueNumber = process.env.MAX_ISSUE_NUMBER ? parseInt(process.env.MAX_ISSUE_NUMBER, 10) : Infinity;
  const minIssueNumber = parseInt(process.env.MIN_ISSUE_NUMBER || "1", 10);
  const limit = parseInt(process.env.LIMIT || "100", 10);
  // A dedupe run takes about 2.5 minutes, so this keeps about five running at once and
  // leaves the rest of the Actions runners and the Claude subscription free for other work.
  const dispatchIntervalMs = 30_000;
  
  console.log(`[DEBUG] Repository: ${owner}/${repo}`);
  console.log(`[DEBUG] Dry run mode: ${dryRun}`);
  console.log(`[DEBUG] Looking at open issues between #${minIssueNumber} and #${maxIssueNumber}, triggering at most ${limit}`);

  console.log(`[DEBUG] Fetching issues between #${minIssueNumber} and #${maxIssueNumber}...`);
  const allIssues: GitHubIssue[] = [];
  let page = 1;
  const perPage = 100;
  
  while (true) {
    const pageIssues: GitHubIssue[] = await githubRequest(
      `/repos/${owner}/${repo}/issues?state=open&per_page=${perPage}&page=${page}&sort=created&direction=desc`,
      token
    );
    
    if (pageIssues.length === 0) break;
    
    // Filter to only include issues within the specified range; the issues endpoint also lists pull requests
    const filteredIssues = pageIssues.filter(issue => 
      !issue.pull_request && issue.number >= minIssueNumber && issue.number < maxIssueNumber
    );
    allIssues.push(...filteredIssues);
    
    // If the oldest issue in this page is still above our minimum, we need to continue
    // but if the oldest issue is below our minimum, we can stop
    const oldestIssueInPage = pageIssues[pageIssues.length - 1];
    if (oldestIssueInPage && oldestIssueInPage.number >= maxIssueNumber) {
      console.log(`[DEBUG] Oldest issue in page #${page} is #${oldestIssueInPage.number}, continuing...`);
    } else if (oldestIssueInPage && oldestIssueInPage.number < minIssueNumber) {
      console.log(`[DEBUG] Oldest issue in page #${page} is #${oldestIssueInPage.number}, below minimum, stopping`);
      break;
    } else if (filteredIssues.length === 0 && pageIssues.length > 0) {
      console.log(`[DEBUG] No issues in page #${page} are in range #${minIssueNumber}-#${maxIssueNumber}, continuing...`);
    }
    
    page++;
    
    // Safety limit to avoid infinite loops
    if (page > 200) {
      console.log("[DEBUG] Reached page limit, stopping pagination");
      break;
    }
  }
  
  console.log(`[DEBUG] Found ${allIssues.length} issues between #${minIssueNumber} and #${maxIssueNumber}`);

  let processedCount = 0;
  let candidateCount = 0;
  let triggeredCount = 0;
  let lowestTriggered: number | undefined;

  for (const issue of allIssues) {
    if (triggeredCount >= limit) {
      console.log(`[DEBUG] Reached the limit of ${limit} workflows, stopping`);
      break;
    }
    processedCount++;
    console.log(
      `[DEBUG] Processing issue #${issue.number} (${processedCount}/${allIssues.length}): ${issue.title}`
    );

    console.log(`[DEBUG] Fetching comments for issue #${issue.number}...`);
    const comments: GitHubComment[] = await githubRequest(
      `/repos/${owner}/${repo}/issues/${issue.number}/comments?per_page=100`,
      token
    );
    console.log(
      `[DEBUG] Issue #${issue.number} has ${comments.length} comments`
    );

    // Look for existing duplicate detection comments (from the dedupe bot)
    const dupeDetectionComments = comments.filter(
      (comment) =>
        comment.body.includes("Found") &&
        comment.body.includes("possible duplicate") &&
        comment.user.type === "Bot"
    );

    console.log(
      `[DEBUG] Issue #${issue.number} has ${dupeDetectionComments.length} duplicate detection comments`
    );

    // Skip if there's already a duplicate detection comment
    if (dupeDetectionComments.length > 0) {
      console.log(
        `[DEBUG] Issue #${issue.number} already has duplicate detection comment, skipping`
      );
      continue;
    }

    candidateCount++;
    const issueUrl = `https://github.com/${owner}/${repo}/issues/${issue.number}`;
    
    try {
      console.log(
        `[INFO] ${dryRun ? '[DRY RUN] ' : ''}Triggering dedupe workflow for issue #${issue.number}: ${issueUrl}`
      );
      await triggerDedupeWorkflow(owner, repo, issue.number, token, dryRun);
      
      if (!dryRun) {
        console.log(
          `[SUCCESS] Successfully triggered dedupe workflow for issue #${issue.number}`
        );
      }
      triggeredCount++;
      lowestTriggered = issue.number;
    } catch (error) {
      console.error(
        `[ERROR] Failed to trigger workflow for issue #${issue.number}: ${error}`
      );
    }

    if (!dryRun) {
      await new Promise(resolve => setTimeout(resolve, dispatchIntervalMs));
    }
  }

  console.log(
    `[DEBUG] Script completed. Processed ${processedCount} issues, found ${candidateCount} candidates without duplicate comments, ${dryRun ? 'would trigger' : 'triggered'} ${triggeredCount} workflows`
  );
  // Issues with no duplicates get no comment, so a later run would pick them again; continue below this batch instead.
  if (lowestTriggered !== undefined) {
    console.log(`[INFO] To continue with older issues, run again with max_issue_number set to ${lowestTriggered}`);
  }
}

backfillDuplicateComments().catch(console.error);

// Make it a module
export {};